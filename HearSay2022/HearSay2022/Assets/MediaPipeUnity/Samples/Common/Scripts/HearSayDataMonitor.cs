using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Networking;

namespace HearSay
{
    // Plain receiver/debug screen. Does not start camera, MediaPipe or Whisper.
    public class HearSayDataMonitor : MonoBehaviour
    {
        private string url, raw = "No response yet.", state = "Connecting…";
        private string sound = "—", sign = "—", source = "—";
        private readonly List<string> history = new List<string>();
        private Vector2 scroll;
        private int responses;
        private float lastResponse = -1f;
        private string lastSummary;
        [Serializable] private class Packet
        {
            public bool soundActive, aslDetected, stale;
            public string soundDirection, aslWord, source;
            public float aslConfidence, ageSeconds;
        }

        public void Initialize(string endpoint) { url = endpoint; StartCoroutine(Receive()); }

        private IEnumerator Receive()
        {
            while (true)
            {
                using (var request = UnityWebRequest.Get(url))
                {
                    request.timeout = 3;
                    UnityWebRequestAsyncOperation operation = null;
                    try { operation = request.SendWebRequest(); }
                    catch (Exception e) { state = "Connection error: " + e.Message; }
                    if (operation != null) yield return operation;
                    if (operation != null && request.result == UnityWebRequest.Result.Success)
                    {
                        raw = request.downloadHandler.text;
                        try
                        {
                            var data = JsonUtility.FromJson<Packet>(raw);
                            if (data == null || !raw.TrimStart().StartsWith("{")) throw new FormatException("Expected JSON object");
                            responses++;
                            lastResponse = Time.unscaledTime;
                            source = data.source ?? "unspecified";
                            state = data.stale ? "STALE — sensor sender stopped (server still reachable)" : "Connected";
                            sound = data.soundActive && !data.stale ? (data.soundDirection ?? "missing direction").ToUpperInvariant() : "OFF";
                            sign = data.aslDetected && !data.stale
                                ? (data.aslWord ?? "missing word").Replace('_', ' ') + "  (" + data.aslConfidence.ToString("P0") + ")"
                                : "No ASL detected";
                            string summary = state + " | source=" + source + " | sound=" + sound + " | ASL=" + sign;
                            if (summary != lastSummary)
                            {
                                history.Insert(0, DateTime.Now.ToString("HH:mm:ss") + "  " + summary);
                                if (history.Count > 40) history.RemoveAt(history.Count - 1);
                                lastSummary = summary;
                            }
                        }
                        catch (Exception e) { state = "Invalid response: " + e.Message; sound = sign = "Unavailable"; }
                    }
                    else if (operation != null)
                    {
                        state = "DISCONNECTED: " + request.error;
                        sound = sign = "Unavailable (last JSON shown below)";
                    }
                }
                yield return new WaitForSecondsRealtime(0.25f);
            }
        }

        private void OnGUI()
        {
            var oldMatrix = GUI.matrix;
            var oldColor = GUI.color;
            GUI.color = HearSayTheme.Navy;
            GUI.DrawTexture(new Rect(0, 0, Screen.width, Screen.height), Texture2D.whiteTexture);
            GUI.color = oldColor;
            Rect safe = Screen.safeArea;
            float scale = Mathf.Min(safe.width / 1000f, safe.height / 650f);
            GUI.matrix = Matrix4x4.TRS(new Vector3(safe.x + (safe.width - 1000 * scale) / 2,
                Screen.height - safe.yMax + (safe.height - 650 * scale) / 2, 0), Quaternion.identity, Vector3.one * scale);
            GUI.Label(new Rect(28, 15, 700, 50), "Data monitor", HearSayTheme.Label(30, Color.white));
            if (HearSayTheme.Action(new Rect(815, 20, 155, 48), "Home", 20)) Destroy(this);
            GUI.Label(new Rect(28, 70, 944, 44), state, HearSayTheme.Label(19, HearSayTheme.Accent));
            GUI.Label(new Rect(28, 114, 944, 36), url, HearSayTheme.Label(17, HearSayTheme.Muted));
            string age = lastResponse < 0 ? "never" : (Time.unscaledTime - lastResponse).ToString("F1") + "s ago";
            GUI.Label(new Rect(28, 155, 944, 32), "Source: " + source + "   Responses: " + responses + "   Last received: " + age,
                HearSayTheme.Label(17, HearSayTheme.Muted));
            GUI.Label(new Rect(28, 193, 944, 40), "SOUND: " + sound, HearSayTheme.Label(25, Color.white));
            GUI.Label(new Rect(28, 236, 944, 40), "ASL: " + sign, HearSayTheme.Label(25, Color.white));
            var label = HearSayTheme.Label(17, Color.white);
            label.richText = false;
            string content = "LATEST JSON\n" + raw + "\n\nCHANGE HISTORY (newest first)\n" + string.Join("\n", history);
            float height = Mathf.Max(310, label.CalcHeight(new GUIContent(content), 906) + 24);
            scroll = GUI.BeginScrollView(new Rect(28, 294, 944, 326), scroll, new Rect(0, 0, 916, height));
            GUI.Label(new Rect(8, 8, 900, height - 16), content, label);
            GUI.EndScrollView();
            GUI.matrix = oldMatrix;
        }
    }
}
