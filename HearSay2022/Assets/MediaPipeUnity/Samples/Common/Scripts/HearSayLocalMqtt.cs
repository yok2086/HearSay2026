using System;
using System.Collections;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using MQTTnet.Client;
using UnityEngine;
using UnityEngine.Networking;

namespace HearSay
{
    public sealed class HearSayLocalMqtt : MonoBehaviour
    {
        private static HearSayLocalMqtt instance;
        private readonly object gate = new object();
        private string pending, endpoint, topic;
        private volatile string connection = "MQTT: connecting";
        private volatile bool connected;
        private CancellationTokenSource cancellation;
        private IMqttClient client;
        private string latest;
        private float receivedAt = -100f;
        private const string Empty = "{\"soundActive\":false,\"aslDetected\":false,\"stale\":true,\"source\":\"mqtt\"}";

        public static bool IsMqtt(string url) => url != null && url.StartsWith("mqtt://", StringComparison.OrdinalIgnoreCase);
        public static bool Valid(string url) => Uri.TryCreate(url, UriKind.Absolute, out var u) && u.Scheme == "mqtt" && !string.IsNullOrEmpty(u.Host) && string.IsNullOrEmpty(u.UserInfo) && u.AbsolutePath.Length > 1 && !u.AbsolutePath.Contains("#") && !u.AbsolutePath.Contains("+") && string.IsNullOrEmpty(u.Query) && string.IsNullOrEmpty(u.Fragment);

        public static HearSayLocalMqtt For(string url)
        {
            if (!Valid(url)) throw new ArgumentException("Use mqtt://HOST:1883/hearsay/demo/status");
            if (instance != null && instance.endpoint == url) return instance;
            if (instance != null) { instance.cancellation.Cancel(); Destroy(instance.gameObject); }
            var go = new GameObject("HearSay Local MQTT");
            DontDestroyOnLoad(go);
            instance = go.AddComponent<HearSayLocalMqtt>();
            instance.endpoint = url;
            var uri = new Uri(url);
            instance.topic = uri.AbsolutePath.TrimStart('/');
            instance.cancellation = new CancellationTokenSource();
            _ = instance.Run(uri, instance.cancellation.Token);
            return instance;
        }

        public static void Stop()
        {
            if (instance == null) return;
            instance.cancellation?.Cancel();
            Destroy(instance.gameObject);
            instance = null;
        }

        private async Task Run(Uri uri, CancellationToken token)
        {
            using (var mqtt = new MqttFactory().CreateMqttClient())
            {
                client = mqtt;
                mqtt.ApplicationMessageReceivedAsync += args => { var message = args.ApplicationMessage; if (!message.Retain && message.Topic == topic && message.PayloadSegment.Count <= 8192) { var bytes = message.PayloadSegment; if (bytes.Array != null) lock (gate) pending = Encoding.UTF8.GetString(bytes.Array, bytes.Offset, bytes.Count); } return Task.CompletedTask; };
                while (!token.IsCancellationRequested)
                {
                    try
                    {
                        connection = "MQTT: connecting";
                        var options = new MqttClientOptionsBuilder().WithClientId("hearsay-" + Guid.NewGuid().ToString("N")).WithTcpServer(uri.Host, uri.Port > 0 ? uri.Port : 1883).WithCleanSession().WithTimeout(TimeSpan.FromSeconds(5)).WithKeepAlivePeriod(TimeSpan.FromSeconds(5)).Build();
                        await mqtt.ConnectAsync(options, token);
                        var result = await mqtt.SubscribeAsync(new MqttClientSubscribeOptionsBuilder().WithTopicFilter(topic).Build(), token);
                        connected = true;
                        connection = "MQTT: connected; waiting for terminal data";
                        while (mqtt.IsConnected) await Task.Delay(250, token);
                    }
                    catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
                    catch (Exception e) { connection = "MQTT: " + e.Message; }
                    finally { connected = false; lock (gate) pending = null; }
                    try { await Task.Delay(2000, token); } catch (OperationCanceledException) { break; }
                }
                client = null;
            }
        }

        [Serializable] private class Packet { public bool soundActive, aslDetected; public string soundDirection, aslWord; public float aslConfidence; }
        private void Update()
        {
            string raw; lock (gate) { raw = pending; pending = null; }
            if (!connected) { latest = null; return; }
            if (raw == null) return;
            try { if (!raw.TrimStart().StartsWith("{")) throw new FormatException(); var packet = JsonUtility.FromJson<Packet>(raw); if (packet == null) throw new FormatException(); latest = raw; receivedAt = Time.realtimeSinceStartup; connection = "MQTT: connected"; } catch { latest = null; connection = "MQTT: invalid status JSON"; }
        }
        public string Read(out string error) { bool fresh = connected && latest != null && Time.realtimeSinceStartup - receivedAt < 4f; error = fresh ? null : connection + " (no fresh status; effects cleared)"; return fresh ? latest : Empty; }
        public async Task Report(string suffix, string json)
        {
            var mqtt = client; if (mqtt == null || !connected) return;
            try { using (var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2))) await mqtt.PublishAsync(new MqttApplicationMessageBuilder().WithTopic(topic + "/" + suffix).WithPayload(json).WithRetainFlag(false).Build(), timeout.Token); } catch { }
        }
        private void OnDestroy() { cancellation?.Cancel(); if (instance == this) instance = null; }
    }

    public sealed class HearSayStatusRequest : IDisposable
    {
        private readonly UnityWebRequest http;
        public int timeout { set { if (http != null) http.timeout = value; } }
        public UnityWebRequest.Result result { get; private set; }
        public string error { get; private set; }
        public sealed class Body { public string text; }
        public Body downloadHandler = new Body();
        private HearSayStatusRequest(string endpoint) { if (!HearSayLocalMqtt.IsMqtt(endpoint)) http = UnityWebRequest.Get(endpoint); }
        public static HearSayStatusRequest Get(string endpoint) => new HearSayStatusRequest(endpoint);
        public IEnumerator SendWebRequest()
        {
            if (http != null) { var operation = http.SendWebRequest(); yield return operation; result = http.result; error = http.error; downloadHandler.text = http.downloadHandler.text; }
            else { downloadHandler.text = HearSayLocalMqtt.For("mqtt://localhost:1883/hearsay/demo/status").Read(out var diagnostic); error = diagnostic; result = UnityWebRequest.Result.Success; }
        }
        public void Dispose() { http?.Dispose(); }
    }
}
