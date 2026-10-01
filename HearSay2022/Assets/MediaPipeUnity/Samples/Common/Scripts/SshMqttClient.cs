using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using MQTTnet.Client;
using MQTTnet.Formatter;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Renci.SshNet;
using Renci.SshNet.Common;

namespace CG4002.Comms
{
    [Serializable]
    public sealed class ConnectionSecrets
    {
        public string wifiSsid = "eduroam";
        public string wifiUsername = UnitySecrets.WifiUsername;
        public string wifiPassword = UnitySecrets.WifiPassword;

        public string jumpHost = "stujump.comp.nus.edu.sg";
        public int jumpPort = 22;
        public string socUsername = UnitySecrets.SocUsername;
        public string socPassword = UnitySecrets.SocPassword;

        public string ultra96Host = "makerslab-fpga-36.ddns.comp.nus.edu.sg";
        public int ultra96Port = 22;
        public string ultra96Username = "xilinx", ultra96Password = "xilinx";

        public string mqttUsername = UnitySecrets.MqttUsername;
        public string mqttPassword = UnitySecrets.MqttPassword;
        public string sshPrivateKey = UnitySecrets.SshPrivateKey;
        public string sshKeyPassphrase = UnitySecrets.SshKeyPassphrase;

        public void Validate()
        {
            if (new[] { jumpHost, socUsername, socPassword, ultra96Host, ultra96Username,
                ultra96Password, mqttUsername, mqttPassword, sshPrivateKey }.Any(string.IsNullOrEmpty))
                throw new ArgumentException("Connection settings are incomplete. Run setup.sh first.");
            if (jumpPort < 1 || jumpPort > 65535 || ultra96Port < 1 || ultra96Port > 65535)
                throw new ArgumentException("SSH ports must be between 1 and 65535.");
        }
    }

    // One complete MQTT application message, independent of TCP fragmentation.
    public sealed class ReceivedMessage
    {
        public readonly string ModelId, Topic, Text;
        public readonly byte[] Payload;
        public readonly DateTime ReceivedAtUtc;
        public ReceivedMessage(string modelId, string topic, byte[] payload, string text)
        {
            ModelId = modelId; Topic = topic; Payload = payload; Text = text;
            ReceivedAtUtc = DateTime.UtcNow;
        }
    }

    public sealed class TelemetryPacket
    {
        public uint Sequence, TimestampMs;
        public short AccelX, AccelY, AccelZ, GyroX, GyroY, GyroZ;
        public ushort Hall;
        public byte Flags;
        public bool HasCrc;

        public static TelemetryPacket Decode(byte[] bytes)
        {
            if (bytes.Length != 23 && bytes.Length != 27)
                throw new InvalidDataException("Telemetry must contain 23 or 27 bytes.");
            using (var reader = new BinaryReader(new MemoryStream(bytes)))
            {
                var packet = new TelemetryPacket {
                    Sequence = reader.ReadUInt32(), TimestampMs = reader.ReadUInt32(),
                    AccelX = reader.ReadInt16(), AccelY = reader.ReadInt16(), AccelZ = reader.ReadInt16(),
                    GyroX = reader.ReadInt16(), GyroY = reader.ReadInt16(), GyroZ = reader.ReadInt16(),
                    Hall = reader.ReadUInt16(), Flags = reader.ReadByte(), HasCrc = bytes.Length == 27
                };
                if (packet.HasCrc)
                {
                    uint crc = 0xffffffff;
                    for (int i = 0; i < 23; i++)
                    {
                        crc ^= bytes[i];
                        for (int bit = 0; bit < 8; bit++)
                            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320u : 0u);
                    }
                    if ((crc ^ 0xffffffff) != reader.ReadUInt32())
                        throw new InvalidDataException("Telemetry CRC mismatch.");
                }
                return packet;
            }
        }

        public override string ToString() =>
            $"seq={Sequence} device_ms={TimestampMs} CRC={(HasCrc ? "OK" : "legacy") }\n" +
            $"accel(raw): {AccelX}, {AccelY}, {AccelZ}\ngyro(raw): {GyroX}, {GyroY}, {GyroZ}\n" +
            $"hall={Hall} flags=0x{Flags:x2}";
    }

    // No Unity dependency: consume messages on Unity's main thread via TryDequeue/Snapshot.
    public sealed class SshMqttClient
    {
        readonly ConnectionSecrets secrets;
        readonly object gate = new object();
        readonly SemaphoreSlim publishGate = new SemaphoreSlim(1, 1);
        readonly Dictionary<string, ReceivedMessage> latest = new Dictionary<string, ReceivedMessage>();
        readonly Queue<ReceivedMessage> pending = new Queue<ReceivedMessage>();
        readonly string clientId = "unity-" + Guid.NewGuid().ToString("N").Substring(0, 16);
        IMqttClient mqtt;
        string status = "Idle";
        long rejected, dropped;
        int running;
        string lastError;
        public string Status { get { lock (gate) return status; } }
        public string LastError { get { lock (gate) return lastError; } }
        public long RejectedMessages { get { lock (gate) return rejected; } }
        public long DroppedMessages { get { lock (gate) return dropped; } }
        public SshMqttClient(ConnectionSecrets settings) { settings.Validate(); secrets = settings; }
        void SetStatus(string text) { lock (gate) status = text; }
        public ReceivedMessage[] Snapshot()
        { lock (gate) return latest.Values.OrderBy(x => x.ModelId).ThenBy(x => x.Topic).ToArray(); }
        public bool TryDequeue(out ReceivedMessage message)
        {
            lock (gate) { message = pending.Count == 0 ? null : pending.Dequeue(); return message != null; }
        }

        SshClient CreateSsh(string host, int port, string username, string password, PrivateKeyFile key)
        {
            var interactive = new KeyboardInteractiveAuthenticationMethod(username);
            interactive.AuthenticationPrompt += (_, args) => {
                foreach (var prompt in args.Prompts)
                    if (prompt.Request.IndexOf("password", StringComparison.OrdinalIgnoreCase) >= 0)
                        prompt.Response = password;
            };
            var info = new ConnectionInfo(host, port, username,
                new PrivateKeyAuthenticationMethod(username, key),
                new PasswordAuthenticationMethod(username, password), interactive) {
                Timeout = TimeSpan.FromSeconds(20)
            };
            // Keep the existing device policy: key + password; no host-key pinning.
            return new SshClient(info) { KeepAliveInterval = TimeSpan.FromSeconds(10) };
        }

        public async Task RunAsync(CancellationToken cancellation)
        {
            if (Interlocked.Exchange(ref running, 1) != 0)
                throw new InvalidOperationException("This client already has a running connection.");
            try
            {
                while (!cancellation.IsCancellationRequested)
                {
                    try
                    {
                        SetStatus("Connecting to SSH jump host...");
                        using (var stream = new MemoryStream(Encoding.UTF8.GetBytes(secrets.sshPrivateKey)))
                        using (var key = string.IsNullOrEmpty(secrets.sshKeyPassphrase)
                            ? new PrivateKeyFile(stream) : new PrivateKeyFile(stream, secrets.sshKeyPassphrase))
                        using (var jump = CreateSsh(secrets.jumpHost, secrets.jumpPort,
                            secrets.socUsername, secrets.socPassword, key))
                        {
                            // Blocking Connect on a worker thread: on Android (IL2CPP) ConnectAsync
                            // timed out in ReceiveAsync waiting for the server banner.
                            await Task.Run(() => jump.Connect(), cancellation).ConfigureAwait(false);
                            using (var sshPort = new ForwardedPortLocal("127.0.0.1", 0,
                                secrets.ultra96Host, (uint)secrets.ultra96Port))
                            {
                                jump.AddForwardedPort(sshPort); sshPort.Start();
                                SetStatus("Connecting to Ultra96 SSH...");
                                using (var ultra = CreateSsh("127.0.0.1", (int)sshPort.BoundPort,
                                    secrets.ultra96Username, secrets.ultra96Password, key))
                                {
                                    await Task.Run(() => ultra.Connect(), cancellation).ConfigureAwait(false);
                                    using (var brokerPort = new ForwardedPortLocal("127.0.0.1", 0, "127.0.0.1", 1883))
                                    {
                                        ultra.AddForwardedPort(brokerPort); brokerPort.Start();
                                        await ReceiveAsync(jump, ultra, (int)brokerPort.BoundPort, cancellation).ConfigureAwait(false);
                                    }
                                }
                            }
                        }
                    }
                    catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { break; }
                    catch (SshAuthenticationException)
                    { SetStatus("SSH authentication failed. Correct settings and retry."); return; }
                    catch (Exception error)
                    {
                        var root = error.GetBaseException();
                        lock (gate) lastError = error.ToString();
                        SetStatus("Disconnected: " + root.GetType().Name + ": " + root.Message + " (retrying)");
                    }
                    try { await Task.Delay(3000, cancellation).ConfigureAwait(false); }
                    catch (OperationCanceledException) { break; }
                }
                SetStatus("Stopped");
            }
            finally { Interlocked.Exchange(ref running, 0); }
        }

        async Task ReceiveAsync(SshClient jump, SshClient ultra, int port, CancellationToken cancellation)
        {
            using (var connection = new MqttFactory().CreateMqttClient())
            {
                connection.ApplicationMessageReceivedAsync += args => {
                    // Latency probe from Comms/stats.py: echo straight back so the Mac
                    // can time the broker <-> phone leg on its own clock.
                    if (args.ApplicationMessage.Topic == LatencyPingTopic)
                        return EchoLatencyPingAsync(connection, args.ApplicationMessage.PayloadSegment);
                    var ack = IsResultTopic(args.ApplicationMessage.Topic)
                        ? AckResultAsync(connection, args.ApplicationMessage.Topic, args.ApplicationMessage.PayloadSegment)
                        : Task.CompletedTask;
                    Store(args.ApplicationMessage.Topic, args.ApplicationMessage.PayloadSegment);
                    return ack;
                };
                var options = new MqttClientOptionsBuilder().WithClientId(clientId)
                    .WithTcpServer("127.0.0.1", port).WithCredentials(secrets.mqttUsername, secrets.mqttPassword)
                    .WithProtocolVersion(MqttProtocolVersion.V311).WithCleanSession()
                    .WithKeepAlivePeriod(TimeSpan.FromSeconds(15)).Build();
                try
                {
                    SetStatus("Connecting to MQTT...");
                    await connection.ConnectAsync(options, cancellation).ConfigureAwait(false);
                    var subscriptions = new MqttClientSubscribeOptionsBuilder()
                        .WithTopicFilter("sys/+/status").WithTopicFilter("tele/+/raw")
                        .WithTopicFilter("tele/+/result").WithTopicFilter("cmd/+/ack")
                        .WithTopicFilter(LatencyPingTopic).Build();
                    var result = await connection.SubscribeAsync(subscriptions, cancellation).ConfigureAwait(false);
                    if (result.Items.Any(x => (int)x.ResultCode >= 128))
                        throw new IOException("Broker rejected a subscription.");
                    lock (gate) { mqtt = connection; status = "Connected over SSH"; }
                    while (connection.IsConnected && jump.IsConnected && ultra.IsConnected)
                        await Task.Delay(500, cancellation).ConfigureAwait(false);
                    throw new IOException("Connection closed.");
                }
                finally
                {
                    await publishGate.WaitAsync().ConfigureAwait(false);
                    try
                    {
                        lock (gate) mqtt = null;
                        using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2)))
                            if (connection.IsConnected)
                                try { await connection.DisconnectAsync(new MqttClientDisconnectOptions(), timeout.Token).ConfigureAwait(false); }
                                catch (Exception) { /* Connection is being disposed regardless. */ }
                    }
                    finally { publishGate.Release(); }
                }
            }
        }

        const string LatencyPingTopic = "latency/phone/ping";

        async Task EchoLatencyPingAsync(IMqttClient connection, ArraySegment<byte> payload)
        {
            try
            {
                await connection.PublishAsync(new MqttApplicationMessageBuilder()
                    .WithTopic("latency/phone/" + clientId + "/pong").WithPayload(payload.ToArray())
                    .WithRetainFlag(false).Build()).ConfigureAwait(false);
            }
            catch (Exception) { /* A lost echo only drops one latency sample. */ }
        }

        static bool IsResultTopic(string topic)
        {
            var parts = (topic ?? "").Split('/');
            return parts.Length == 3 && parts[0] == "tele" && parts[2] == "result";
        }

        // Acknowledges every received word (RESULT) on phone/<clientId>/ack as soon as it
        // arrives, before Unity processes it. Comms/stats.py times RESULT -> ACK to get the
        // Ultra96 <-> phone round trip.
        async Task AckResultAsync(IMqttClient connection, string topic, ArraySegment<byte> payload)
        {
            try
            {
                var seq = JObject.Parse(Encoding.UTF8.GetString(payload.Array, payload.Offset, payload.Count))["seq"];
                if (seq == null) return;
                var ack = new JObject {
                    ["type"] = "ACK", ["device_id"] = clientId,
                    ["ack_topic"] = topic, ["ack_seq"] = seq
                };
                await connection.PublishAsync(new MqttApplicationMessageBuilder()
                    .WithTopic("phone/" + clientId + "/ack").WithPayload(ack.ToString(Formatting.None))
                    .WithRetainFlag(false).Build()).ConfigureAwait(false);
            }
            catch (Exception) { /* Malformed result or lost ack: only one latency sample is dropped. */ }
        }

        static bool ValidModel(string id) => !string.IsNullOrEmpty(id) && id.Length <= 64 &&
            id.All(c => (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_');
        public Task SendHapticAsync(string modelId, CancellationToken cancellation = default) =>
            PublishAsync(modelId, "haptic/cmd", "1", cancellation);
        public Task SendConfigAsync(string modelId, string json, CancellationToken cancellation = default) =>
            PublishAsync(modelId, "set", json, cancellation, topicPrefix: "cmd");

        async Task PublishAsync(string modelId, string suffix, string payload, CancellationToken cancellation, string topicPrefix = "esp32")
        {
            if (!ValidModel(modelId)) throw new ArgumentException("Invalid model ID.");
            if (payload == null || Encoding.UTF8.GetByteCount(payload) > 4096)
                throw new ArgumentException("Command is empty or too large.");
            using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellation))
            {
                timeout.CancelAfter(TimeSpan.FromSeconds(5));
                await publishGate.WaitAsync(timeout.Token).ConfigureAwait(false);
                try
                {
                    IMqttClient connection; lock (gate) connection = mqtt;
                    if (connection == null || !connection.IsConnected) throw new IOException("Not connected.");
                    await connection.PublishAsync(new MqttApplicationMessageBuilder()
                        .WithTopic($"{topicPrefix}/{modelId}/{suffix}").WithPayload(payload)
                        .WithRetainFlag(false).Build(), timeout.Token).ConfigureAwait(false);
                }
                finally { publishGate.Release(); }
            }
        }

        void Store(string topic, ArraySegment<byte> segment)
        {
            // Matches the actual HearSay protocol topics: sys/<id>/status,
            // tele/<id>/raw, tele/<id>/result, cmd/<id>/ack - all JSON text,
            // no binary telemetry format in use.
            var parts = (topic ?? "").Split('/');
            if (parts.Length != 3 || !ValidModel(parts[1]) || segment.Count > 4096 ||
                !((parts[0] == "sys" && parts[2] == "status") ||
                  (parts[0] == "tele" && (parts[2] == "raw" || parts[2] == "result")) ||
                  (parts[0] == "cmd" && parts[2] == "ack")))
            { lock (gate) rejected++; return; }
            var bytes = segment.ToArray();
            string text;
            try { text = Encoding.UTF8.GetString(bytes); }
            catch (Exception) { lock (gate) rejected++; return; }
            var message = new ReceivedMessage(parts[1], topic, bytes, text);
            lock (gate)
            {
                if (!latest.ContainsKey(topic) && latest.Count >= 192) { rejected++; return; }
                latest[topic] = message;
                if (pending.Count >= 256) { pending.Dequeue(); dropped++; }
                pending.Enqueue(message);
            }
        }
    }
}