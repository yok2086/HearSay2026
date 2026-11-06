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

    // ---- Mic direction (firebeetle_LR) -> deaf user's left/right sound waves ----
    public string soundDeviceId = "mic_lr_01";
    public string lastSoundEventId = "";
    private float lastSoundTime = -999f;

    public string ConnectionStatus => client?.Status ?? "Not started";

    // private static readonly System.Collections.Generic.Dictionary<string, string> EventToAslWord =
    //     new System.Collections.Generic.Dictionary<string, string>
    // {
    //     { "hello", "hello" },
    //     { "later", "later" },
    //     { "I", "I" },
    //     { "yes", "yes" },
    //     { "no", "no" },
    //     { "please", "please" },
    //     { "thank", "thank" },
    //     { "what", "what" },
    //     { "eat", "eat" },
    //     { "more", "more" },
    //     { "go", "go" },
    //     { "love", "love" },
    //     { "happy", "happy" },
    //     { "know", "know" },
    //     { "ready", "ready" },
    //     { "deaf", "deaf" },
    //     { "you", "you" },
    //     { "where", "where" },
    //     { "sorry", "sorry" },
    //     { "wait_notyet_", "wait_notyet_" }
    // };

    // private static readonly System.Collections.Generic.HashSet<string> AllowedWords =
    //     new System.Collections.Generic.HashSet<string>
    // {
    //        "hello", "later", "I", "yes", "no", "please", "thank", "what", "eat", "more",
    // "go", "love", "happy", "know", "ready", "deaf", "you", "where", "sorry", "wait_notyet_",
    // };

    private static readonly System.Collections.Generic.Dictionary<string, string> EventToAslWord =
        new System.Collections.Generic.Dictionary<string, string>
    {
        { "hello", "hello" },
        { "see_you_later", "see_you_later" },
        { "me", "me" },
        { "yes", "yes" },
        { "no", "no" },
        { "help", "help" },
        { "please", "please" },
        { "thank_you", "thank_you" },
        { "want", "want" },
        { "what", "what" },
        { "again", "again" },
        { "eat_food", "eat_food" },
        { "more", "more" },
        { "go_to", "go_to" },
        { "toilet", "toilet" },
        { "fine", "fine" },
        { "like", "like" },
        { "learn", "learn" },
        { "sign", "sign" },
        { "finish", "finish" }
    };

    // HandleMessage lowercases before checking this set, so every entry must be lowercase.
    private static readonly System.Collections.Generic.HashSet<string> AllowedWords =
        new System.Collections.Generic.HashSet<string>
    {
        "hello", "see_you_later", "me", "yes", "no", "help", "please", "thank_you", "want", "what",
        "again", "eat_food", "more", "go_to", "toilet", "fine", "like", "learn", "sign", "finish",
    };

    public float staleAfterSeconds = 10f;
    private float lastRealDataTime = -999f;

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

        float ageSeconds = Time.time - lastRealDataTime;
        if (ageSeconds > staleAfterSeconds)
        {
            aslDetected = false;
        }
        // The mic has its own timer so it isn't cleared just because no ASL result arrived.
        if (Time.time - lastSoundTime > staleAfterSeconds)
        {
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
        if (type == "TELEMETRY" && msg.ModelId == soundDeviceId) { HandleSoundDirection(payload); return; }
        if (type == "TELEMETRY") { StoreFlex(msg.ModelId, payload); return; }
        if (type == "CALIBRATION_ACK") { StoreCalibration(msg.ModelId, payload); return; }
        if (type == "CALIBRATION_STATUS" || type == "CALIBRATION") { StoreGloveCalibration(msg.ModelId, type, payload); return; }
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

    // firebeetle_LR sends one TELEMETRY per speech event with direction
    // LEFT / RIGHT / FRONT / UNCERTAIN. Only LEFT and RIGHT drive the sound waves.
    void HandleSoundDirection(JObject payload)
    {
        string direction = payload["payload"]?["direction"]?.ToString()?.Trim().ToLowerInvariant();
        if (direction != "left" && direction != "right") return;

        soundActive = true;
        soundDirection = direction;
        // A new id per event lets HearSayDemoVisuals replay the waves for repeat same-side sounds.
        lastSoundEventId = payload["seq"]?.ToString() ?? System.Guid.NewGuid().ToString();
        lastSoundTime = Time.time;
    }

    // ---------------- Glove calibration support ----------------
    // Used by HearSayGloveCalibration ("Calibrate glove" on the ASL translation screen).

    public static readonly string[] GloveIds = { "glove_01L", "glove_01R" };

    /// The glove's own calibration (firmware runs it when it connects):
    /// CALIBRATION_STATUS on sys/<glove>/status (stage START / STRAIGHT / FIST /
    /// FAILED / SUCCESS) and the retained CALIBRATION readings on sys/<glove>/calibration.
    public class GloveCalibrationState
    {
        public string stage = "";
        public string message = "";
        public bool calibrated;
        public float updatedAt;
    }

    readonly System.Collections.Generic.Dictionary<string, GloveCalibrationState> gloveCalibration =
        new System.Collections.Generic.Dictionary<string, GloveCalibrationState>();

    void StoreGloveCalibration(string gloveId, string type, JObject message)
    {
        if (!gloveCalibration.TryGetValue(gloveId, out var state))
            gloveCalibration[gloveId] = state = new GloveCalibrationState();
        if (type == "CALIBRATION")
        {
            state.calibrated = true;
            if (state.stage == "" || state.stage == "START") { state.stage = "SUCCESS"; state.message = "Calibrated"; }
        }
        else
        {
            state.stage = message["payload"]?["stage"]?.ToString() ?? "";
            state.message = message["payload"]?["message"]?.ToString() ?? "";
            if (state.stage == "SUCCESS") state.calibrated = true;
            else if (state.stage == "START") state.calibrated = false;
        }
        state.updatedAt = Time.time;
        Debug.Log($"HearSaySshMqttManager: {gloveId} calibration {state.stage}: {state.message}");
    }

    public bool TryGetGloveCalibration(string gloveId, out GloveCalibrationState state) =>
        gloveCalibration.TryGetValue(gloveId, out state);
    public bool IsConnected => ConnectionStatus == "Connected over SSH";

    /// Whisper speech-to-text API on the Ultra96 (POST a WAV, get {"text": ...}),
    /// reached through this SSH tunnel. Null while not connected.
    public string WhisperApiUrl
    {
        get
        {
            int port = client != null ? client.WhisperApiPort : 0;
            // Only needs the SSH tunnel, not MQTT, so captions work even if the broker is down.
            return port > 0 ? $"http://127.0.0.1:{port}/inference" : null;
        }
    }
    public string LastCommandError { get; private set; }

    readonly System.Collections.Generic.Dictionary<string, int[]> latestFlex =
        new System.Collections.Generic.Dictionary<string, int[]>();
    readonly System.Collections.Generic.Dictionary<string, float> latestFlexTime =
        new System.Collections.Generic.Dictionary<string, float>();
    readonly System.Collections.Generic.Dictionary<string, int[]> calibrationBaseline =
        new System.Collections.Generic.Dictionary<string, int[]>();
    readonly System.Collections.Generic.Dictionary<string, float> calibrationTime =
        new System.Collections.Generic.Dictionary<string, float>();

    void StoreFlex(string gloveId, JObject message)
    {
        var flex = message["payload"]?["flex"] as JArray;
        if (flex == null || flex.Count != 5) return;
        var values = new int[5];
        for (int i = 0; i < 5; i++) values[i] = flex[i].Type == JTokenType.Integer ? (int)flex[i] : 0;
        latestFlex[gloveId] = values;
        latestFlexTime[gloveId] = Time.time;
    }

    static int[] ReadInts(JToken token)
    {
        var values = new int[5];
        if (token is JArray array)
            for (int i = 0; i < Mathf.Min(5, array.Count); i++)
                values[i] = array[i].Type == JTokenType.Integer ? (int)array[i] : 0;
        return values;
    }

    void StoreCalibration(string gloveId, JObject message)
    {
        var payload = message["payload"];
        var values = ReadInts(payload?["flex_baseline"]);
        calibrationBaseline[gloveId] = values;
        calibrationTime[gloveId] = Time.time;

        string step = payload?["step"]?.ToString();
        if (!string.IsNullOrEmpty(step))
        {
            var okFlags = ReadInts(payload["ok"]);
            var ack = new CalibrationStepAck
            {
                step = step,
                values = ReadInts(payload["values"]),
                ok = new bool[5],
                receivedAt = Time.time,
            };
            for (int i = 0; i < 5; i++) ack.ok[i] = okFlags[i] == 1;
            calibrationSteps[gloveId + "/" + step] = ack;
            Debug.Log($"HearSaySshMqttManager: {gloveId} calibration step {step}: values {string.Join(",", ack.values)} ok {string.Join(",", okFlags)}");
        }
        else Debug.Log($"HearSaySshMqttManager: {gloveId} calibrated, baselines {string.Join(",", values)}");
    }

    /// One guided-calibration step reported by a glove (CALIBRATION_ACK with payload.step).
    public class CalibrationStepAck
    {
        public string step;
        public int[] values;   // raw readings (or thresholds for "save")
        public bool[] ok;      // per finger, thumb..pinky
        public float receivedAt;
    }

    readonly System.Collections.Generic.Dictionary<string, CalibrationStepAck> calibrationSteps =
        new System.Collections.Generic.Dictionary<string, CalibrationStepAck>();

    /// The glove's answer to a guided-calibration step sent after sinceTime.
    public bool TryGetCalibrationStep(string gloveId, string step, float sinceTime, out CalibrationStepAck ack)
    {
        return calibrationSteps.TryGetValue(gloveId + "/" + step, out ack) && ack.receivedAt >= sinceTime;
    }

    /// Guided calibration (glove firmware: CALIBRATE with payload.step =
    /// ready / straight / fist / check / save / fail). The glove buzzes, waits
    /// settleMs for the wearer to get into position, measures and answers.
    public void RequestCalibrationStep(string gloveId, string step, int settleMs)
    {
        if (client == null) { LastCommandError = "Not connected"; return; }
        string json = "{\"type\":\"CALIBRATE\",\"device_id\":\"phone\",\"payload\":{\"step\":\"" + step +
                      "\",\"settle_ms\":" + settleMs + "}}";
        _ = SendCommandAsync(gloveId, json);
    }

    /// Latest raw flex readings (0-4095; thumb..pinky) if newer than maxAgeSeconds.
    public bool TryGetFlex(string gloveId, float maxAgeSeconds, out int[] flex)
    {
        flex = null;
        return latestFlexTime.TryGetValue(gloveId, out float t) && Time.time - t <= maxAgeSeconds &&
               latestFlex.TryGetValue(gloveId, out flex);
    }

    /// True once the glove confirmed a calibration that happened after sinceTime.
    public bool TryGetCalibration(string gloveId, float sinceTime, out int[] baseline)
    {
        baseline = null;
        return calibrationTime.TryGetValue(gloveId, out float t) && t >= sinceTime &&
               calibrationBaseline.TryGetValue(gloveId, out baseline);
    }

    /// Asks a glove to re-measure its straight-finger baselines (glove firmware: CALIBRATE).
    public void RequestCalibration(string gloveId)
    {
        if (client == null) { LastCommandError = "Not connected"; return; }
        string json = "{\"type\":\"CALIBRATE\",\"device_id\":\"phone\",\"payload\":{\"step\":\"open_hand\"}}";
        _ = SendCommandAsync(gloveId, json);
    }

    async Task SendCommandAsync(string gloveId, string json)
    {
        try
        {
            await client.SendConfigAsync(gloveId, json, cts.Token);
            LastCommandError = null;
        }
        catch (Exception ex)
        {
            LastCommandError = ex.Message;
            Debug.LogWarning($"HearSaySshMqttManager: could not send CALIBRATE to {gloveId}: {ex.Message}");
        }
    }

    void OnDestroy()
    {
        cts?.Cancel();
    }
}