/*
 * HearSaySshMqttManager.cs
 *
 * Owns an SshMqttClient's lifecycle inside Unity: starts its two-hop SSH
 * tunnel + MQTT connection as a background Task, drains its message queue
 * every frame (Unity's API isn't thread-safe, so parsing happens here in
 * Update(), not inside SshMqttClient's own background thread), and exposes
 * the same five public fields as HearSayMqttDataReceiver.cs / the HTTP
 * /status response - so ASLManager/CaptionManager/etc. don't care which
 * connectivity strategy is underneath.
 *
 * This is the recommended approach over HearSayMqttDataReceiver.cs's direct
 * TLS connection IF the phone will be on a network with client isolation
 * (NUS_STU/eduroam) - the SSH tunnel makes an outbound connection to an
 * external host instead of a direct peer-to-peer connection to the laptop,
 * which is the traffic pattern that actually gets through on this project's
 * campus network.
 */

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
    public string socUsername = "nathan";
    public string socPassword = "";
    public string ultra96Host = "makerslab-fpga-36.ddns.comp.nus.edu.sg";
    public int ultra96Port = 22;
    public string ultra96Username = "xilinx";
    public string ultra96Password = "xilinx";
    public string mqttUsername = "nathan";
    public string mqttPassword = "mqtt";

    [TextArea(4, 10)]
    [Tooltip("Full contents of the phone's registered private key, including -----BEGIN/END----- lines. Register the matching public key via skeys.comp.nus.edu.sg the same way the ESP32's key was registered, or reuse the same key pair.")]
    public string sshPrivateKey = "";
    public string sshKeyPassphrase = "";

    // ---- Same five fields as the HTTP /status response / HearSayMqttDataReceiver.cs ----
    public bool soundActive = false;
    public string soundDirection = "left";
    public bool aslDetected = false;
    public string aslWord = "hello";
    public float aslConfidence = 0f;
    // -------------------------------------------------------------------------------

    // Mirrors DemoStatus.soundEventId in HearSayDemoVisuals.cs - lets that script
    // detect "a genuinely new event happened" even if direction/word repeat.
    // Uses the MQTT message's own seq number, which is already unique per message.
    public string lastResultEventId = "";

    public string ConnectionStatus => client?.Status ?? "Not started";

    // Map YOUR inference model's raw event labels to the fixed 20-word
    // vocabulary - same placeholder as the other receiver files, fill in
    // with your real labels.
    private static readonly System.Collections.Generic.Dictionary<string, string> EventToAslWord =
        new System.Collections.Generic.Dictionary<string, string>
    {
        { "HELLO", "hello" },
        { "THANK_YOU", "thank_you" },
        { "YES", "yes" },
        { "NO", "no" },
        { "HELP", "help" },
        { "PLEASE", "please" },
    };

    private static readonly System.Collections.Generic.HashSet<string> AllowedWords =
        new System.Collections.Generic.HashSet<string>
    {
        "hello", "see_you_later", "i_me", "yes", "no", "help", "please", "thank_you",
        "want", "what", "again_repeat", "eat_food", "more", "go_to", "bathroom",
        "fine", "like", "learn", "sign", "finish_done",
    };

    public float staleAfterSeconds = 10f;
    private float lastRealDataTime = -999f;

    private SshMqttClient client;
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

        // Fire-and-forget: RunAsync loops internally, reconnecting on
        // failure, until cts is cancelled in OnDestroy.
        _ = client.RunAsync(cts.Token);
    }

    void Update()
    {
        if (client == null) return;

        while (client.TryDequeue(out ReceivedMessage msg))
        {
            HandleMessage(msg);
        }

        float ageSeconds = Time.time - lastRealDataTime;
        if (ageSeconds > staleAfterSeconds)
        {
            aslDetected = false;
            soundActive = false;
        }
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
        if (type != "RESULT") return;

        string rawEvent = payload["payload"]?["event"]?.ToString();
        float confidence = Mathf.Clamp01(payload["payload"]?["confidence"]?.ToObject<float>() ?? 0f);

        if (rawEvent == null || !EventToAslWord.TryGetValue(rawEvent, out string mappedWord))
        {
            Debug.LogWarning($"HearSaySshMqttManager: no mapping for event '{rawEvent}'");
            return;
        }

        string normalized = mappedWord.ToLowerInvariant().Trim().Replace("/", "_").Replace(" ", "_");
        if (!AllowedWords.Contains(normalized))
        {
            Debug.LogWarning($"HearSaySshMqttManager: '{normalized}' not in allowed word list");
            return;
        }

        aslDetected = true;
        aslWord = normalized;
        aslConfidence = confidence;
        lastResultEventId = payload["seq"]?.ToString() ?? System.Guid.NewGuid().ToString();
        lastRealDataTime = Time.time;
    }

    void OnDestroy()
    {
        cts?.Cancel();
    }
}
