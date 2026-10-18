using System;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using UnityEngine;
using CG4002.Comms;

public class HearSaySshMqttManager : MonoBehaviour
{
    [Header("SSH / MQTT connection settings")]
    public string jumpHost = "stujump.comp.nus.edu.sg";
    public int jumpPort = 22;
    public string socUsername = UnitySecrets.SocUsername;
    public string socPassword = UnitySecrets.SocPassword;
    public string ultra96Host = "makerslab-fpga-36.ddns.comp.nus.edu.sg";
    public int ultra96Port = 22;
    public string ultra96Username = "xilinx";
    public string ultra96Password = "xilinx";
    public string mqttUsername = UnitySecrets.MqttUsername;
    public string mqttPassword = UnitySecrets.MqttPassword;

    [TextArea(4, 10)]
    [Tooltip("Full contents of the phone's registered private key, including -----BEGIN/END----- lines. Register the matching public key via skeys.comp.nus.edu.sg the same way the ESP32's key was registered, or reuse the same key pair.")]
    public string sshPrivateKey = UnitySecrets.SshPrivateKey;
    public string sshKeyPassphrase = UnitySecrets.SshKeyPassphrase;

    // ---- Same five fields as the HTTP /status response / HearSayMqttDataReceiver.cs ----
    public bool soundActive = false;
    public string soundDirection = "left";
    public bool aslDetected = false;
    public string aslWord = "hello";
    public float aslConfidence = 0f;
    // -------------------------------------------------------------------------------

    public string lastResultEventId = "";
    public string lastSoundEventId = "";

    [Header("Which streams this role consumes")]
    [Tooltip("Glove TCN RESULTs (tele/<id>/result) - the conversation partner shows these.")]
    public bool receiveAsl = true;
    [Tooltip("Mic direction TELEMETRY from soundDeviceId - the deaf user's left/right cue.")]
    public bool receiveSound = true;
    public string soundDeviceId = "mic_lr_01";

    public string ConnectionStatus => client?.Status ?? "Not started";

    // TCN labels (SIGNS in tcn_infer.py) -> display word. Anything not listed is dropped.
    private static readonly System.Collections.Generic.Dictionary<string, string> EventToAslWord =
        new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
    {
        { "hello", "hello" },
        { "later", "see_you_later" },
        { "I", "i_me" },
        { "yes", "yes" },
        { "no", "no" },
        { "please", "please" },
        { "thank", "thank_you" },
        { "what", "what" },
        { "eat", "eat_food" },
        { "more", "more" },
        { "go", "go_to" },
        { "love", "love" },
        { "happy", "happy" },
        { "know", "know" },
        { "ready", "ready" },
        { "deaf", "deaf" },
        { "you", "you" },
        { "where", "where" },
        { "sorry", "sorry" },
        { "wait_notyet_", "wait_not_yet" },
    };

    public float staleAfterSeconds = 10f;
    private float lastRealDataTime = -999f;
    private float lastSoundTime = -999f;

    private SshMqttClient client;
    private string lastLoggedStatus;
    private CancellationTokenSource cts;

    void Start()
    {
        var secrets = new ConnectionSecrets
        {
            jumpHost = jumpHost,
            jumpPort = jumpPort,
            socUsername = socUsername,
            socPassword = socPassword,
            ultra96Host = ultra96Host,
            ultra96Port = ultra96Port,
            ultra96Username = ultra96Username,
            ultra96Password = ultra96Password,
            mqttUsername = mqttUsername,
            mqttPassword = mqttPassword,
            sshPrivateKey = sshPrivateKey,
            sshKeyPassphrase = sshKeyPassphrase,
        };

        client = new SshMqttClient(secrets);
        cts = new CancellationTokenSource();

        _ = ProbeJumpHostAsync(jumpHost, jumpPort);
        _ = client.RunAsync(cts.Token);
    }

    // Diagnostic: plain TCP connect to the jump host, reading the SSH banner.
    // Tells a network-level block apart from an SSH handshake problem.
    static async Task ProbeJumpHostAsync(string host, int port)
    {
        try
        {
            var addresses = await System.Net.Dns.GetHostAddressesAsync(host);
            Debug.Log($"HearSaySshMqttManager: probe DNS {host} -> {string.Join(", ", (object[])addresses)}");
            foreach (var address in addresses)
            {
                using (var tcp = new System.Net.Sockets.TcpClient(address.AddressFamily))
                {
                    var connect = tcp.ConnectAsync(address, port);
                    if (await Task.WhenAny(connect, Task.Delay(10000)) != connect)
                    {
                        Debug.Log($"HearSaySshMqttManager: probe TCP {address}:{port} TIMED OUT (network blocks it)");
                        continue;
                    }
                    await connect;
                    var buffer = new byte[64];
                    var read = tcp.GetStream().ReadAsync(buffer, 0, buffer.Length);
                    string banner = await Task.WhenAny(read, Task.Delay(5000)) == read
                        ? System.Text.Encoding.ASCII.GetString(buffer, 0, read.Result).Trim()
                        : "(no banner within 5s)";
                    Debug.Log($"HearSaySshMqttManager: probe TCP {address}:{port} OK, banner: {banner}");
                }
            }
        }
        catch (Exception ex)
        {
            Debug.Log($"HearSaySshMqttManager: probe failed: {ex}");
        }
    }

    void Update()
    {
        if (client == null) return;

        // Shows up in the Console and in `adb logcat -s Unity`.
        string status = client.Status;
        if (status != lastLoggedStatus)
        {
            Debug.Log($"HearSaySshMqttManager: {status}");
            if (status.StartsWith("Disconnected")) Debug.Log($"HearSaySshMqttManager: detail {client.LastError}");
            lastLoggedStatus = status;
        }

        while (client.TryDequeue(out ReceivedMessage msg))
        {
            HandleMessage(msg);
        }

        if (Time.time - lastRealDataTime > staleAfterSeconds) aslDetected = false;
        if (Time.time - lastSoundTime > staleAfterSeconds) soundActive = false;
    }

    void HandleMessage(ReceivedMessage msg)
    {
        JObject payload;
        try { payload = JObject.Parse(msg.Text); }
        catch (Exception ex)
        {
            Debug.LogWarning($"HearSaySshMqttManager: bad payload on {msg.Topic}: {ex.Message}");
            return;
        }

        string type = payload["type"]?.ToString();
        if (type == "RESULT" && receiveAsl) HandleAslResult(payload);
        else if (type == "TELEMETRY" && receiveSound && msg.ModelId == soundDeviceId) HandleSoundDirection(payload);
    }

    void HandleAslResult(JObject payload)
    {
        string rawEvent = payload["payload"]?["event"]?.ToString();
        float confidence = Mathf.Clamp01(payload["payload"]?["confidence"]?.ToObject<float>() ?? 0f);

        if (rawEvent == null || !EventToAslWord.TryGetValue(rawEvent, out string mappedWord))
        {
            Debug.LogWarning($"HearSaySshMqttManager: no mapping for event '{rawEvent}'");
            return;
        }

        aslDetected = true;
        aslWord = mappedWord;
        aslConfidence = confidence;
        lastResultEventId = payload["seq"]?.ToString() ?? System.Guid.NewGuid().ToString();
        lastRealDataTime = Time.time;
    }

    // firebeetle_LR sends one TELEMETRY per speech event with direction
    // LEFT / RIGHT / FRONT / UNCERTAIN. Only LEFT and RIGHT drive the cue.
    void HandleSoundDirection(JObject payload)
    {
        string direction = payload["payload"]?["direction"]?.ToString()?.Trim().ToLowerInvariant();
        if (direction != "left" && direction != "right") return;

        soundActive = true;
        soundDirection = direction;
        // A new id per event lets HearSayDemoVisuals replay the cue for repeat same-side sounds.
        lastSoundEventId = payload["seq"]?.ToString() ?? System.Guid.NewGuid().ToString();
        lastSoundTime = Time.time;
    }

    void OnDestroy()
    {
        cts?.Cancel();
    }
}