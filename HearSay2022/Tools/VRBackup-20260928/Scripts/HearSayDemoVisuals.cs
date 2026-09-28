using System.Collections;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.UI;

namespace HearSay
{
    // Visual-only demo layer. It does not modify the speaker, segmentation, or Whisper code.
    public class HearSayDemoVisuals : MonoBehaviour
    {
        public static string ActiveServerUrl { get; private set; }
        [Header("Dummy server")]
        [SerializeField] private string serverUrl = "http://192.168.1.2:8080/status";
        [SerializeField] private float pollIntervalSeconds = 0.25f;
        [SerializeField] private bool useRandomFallback = false;
        [SerializeField, Range(1f, 2f)] private float soundCueDuration = 1.5f;
        private float soundCueUntil;
        private bool previousSoundActive;
        private string previousSoundDirection;
        private string previousSoundEventId;

        private Text soundDirectionText;
        private HearSaySoundWaves soundWaves;
        private GameObject uiRoot;
        private Text aslText;
        private Image aslPanel;
        private Text calibrationText;
        private bool showDiagnostics;
        private DemoStatus status = new DemoStatus();
        private float nextRandomStateAt;
        private bool serverHasResponded;
        private string serverStatus = "SERVER: CONNECTING";

        [System.Serializable]
        private class DemoStatus
        {
            public bool soundActive;
            public string soundDirection;
            public string soundEventId;
            public bool aslDetected;
            public string aslWord;
            public float aslConfidence;
        }

        private void Start()
        {
            if (HearSayRoles.DeafRole && !string.IsNullOrEmpty(HearSayRoles.SessionServerUrl))
                serverUrl = HearSayRoles.SessionServerUrl;
            ActiveServerUrl = serverUrl;
            CreateUi();
            StartCoroutine(PollServer());
            StartCoroutine(ReportSpeakerStatus());
        }

        private IEnumerator ReportSpeakerStatus()
        {
            var endpoint = new System.Uri(new System.Uri(serverUrl), "/speaker").AbsoluteUri;
            while (true)
            {
                // A short heartbeat also restores the state after a server restart.
                // The server prints only transitions, not every heartbeat.
                var json = SpeakerActivity.IsSpeaking
                    ? "{\"detected\":true}" : "{\"detected\":false}";
                using (var request = new UnityWebRequest(endpoint, "POST"))
                {
                    request.uploadHandler = new UploadHandlerRaw(System.Text.Encoding.UTF8.GetBytes(json));
                    request.downloadHandler = new DownloadHandlerBuffer();
                    request.SetRequestHeader("Content-Type", "application/json");
                    request.timeout = 2;
                    UnityWebRequestAsyncOperation operation = null;
                    try { operation = request.SendWebRequest(); }
                    catch (System.Exception exception) { Debug.LogWarning("Speaker report: " + exception.Message); }
                    if (operation != null) yield return operation;
                }
                yield return new WaitForSecondsRealtime(0.5f);
            }
        }

        private void Update()
        {
            if (useRandomFallback && !serverHasResponded && Time.unscaledTime >= nextRandomStateAt)
            {
                CreateRandomStatus();
                nextRandomStateAt = Time.unscaledTime + Random.Range(3.5f, 6f);
            }

            // The sound cue guides the user only until the existing speaker detector locks on.
            var speakerDetected = SpeakerActivity.IsSpeaking;
            string direction = (status.soundDirection ?? "").Trim().ToLowerInvariant();
            bool newSound = status.soundActive &&
                (!previousSoundActive || direction != previousSoundDirection ||
                 status.soundEventId != previousSoundEventId);
            if (newSound) soundCueUntil = Time.unscaledTime + soundCueDuration;
            previousSoundActive = status.soundActive;
            previousSoundDirection = direction;
            previousSoundEventId = status.soundEventId;
            // Consume the cue when a speaker is found; don't replay it on losing the face.
            if (speakerDetected || !status.soundActive) soundCueUntil = 0f;
            SetSoundCueVisible(status.soundActive && !speakerDetected && Time.unscaledTime < soundCueUntil);
            SetAslVisible(status.aslDetected && !HearSayRoles.DeafRole);
            UpdateCalibrationReadout();
        }

        private void OnGUI()
        {
            if (HearSayRoles.HomeButton()) HearSayRoles.ReturnToSelector();
            float scale = Mathf.Clamp(UnityEngine.Screen.height / 650f, 0.75f, 2.5f);
            Rect safe = UnityEngine.Screen.safeArea;
            if (HearSayTheme.Action(new Rect(safe.xMin + 16f * scale,
                UnityEngine.Screen.height - safe.yMin - 60f * scale, 110f * scale, 44f * scale),
                showDiagnostics ? "Hide debug" : "Debug", Mathf.RoundToInt(16f * scale)))
                showDiagnostics = !showDiagnostics;
        }

        private IEnumerator PollServer()
        {
            while (true)
            {
                using (var request = UnityWebRequest.Get(serverUrl))
                {
                    request.timeout = 2;
                    UnityWebRequestAsyncOperation operation = null;
                    try
                    {
                        operation = request.SendWebRequest();
                    }
                    catch (System.Exception exception)
                    {
                        serverStatus = "SERVER: " + exception.Message;
                        Debug.LogWarning(serverStatus);
                    }
                    if (operation != null) yield return operation;

                    if (operation != null && request.result == UnityWebRequest.Result.Success)
                    {
                        DemoStatus response = null;
                        try
                        {
                            response = JsonUtility.FromJson<DemoStatus>(request.downloadHandler.text);
                        }
                        catch (System.Exception exception)
                        {
                            serverStatus = "SERVER: INVALID DATA";
                            Debug.LogWarning(exception.Message);
                        }
                        if (response != null)
                        {
                            status = response;
                            serverHasResponded = true;
                            serverStatus = "SERVER: CONNECTED";
                        }
                    }
                    else if (operation != null)
                    {
                        serverStatus = "SERVER: " + request.error;
                    }
                }

                yield return new WaitForSecondsRealtime(pollIntervalSeconds);
            }
        }

        private void CreateRandomStatus()
        {
            var words = new[]
            {
                "hello", "see_you_later", "i_me", "yes", "no", "help", "please", "thank_you",
                "want", "what", "again_repeat", "eat_food", "more", "go_to", "bathroom", "fine",
                "like", "learn", "sign", "finish_done"
            };
            var directions = new[] { "left", "right" };
            status.soundActive = Random.value > 0.2f;
            status.soundDirection = directions[Random.Range(0, directions.Length)];
            status.aslDetected = Random.value > 0.35f;
            status.aslWord = words[Random.Range(0, words.Length)];
            status.aslConfidence = Random.Range(0.78f, 0.98f);
        }

        private void CreateUi()
        {
            var canvasObject = new GameObject("HearSay Demo UI");
            uiRoot = canvasObject;
            var canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 200;
            var scaler = canvasObject.AddComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1920, 1080);

            var wavePrefab = Resources.Load<HearSaySoundWaves>("HearSaySoundWaves");
            if (wavePrefab != null)
                soundWaves = Instantiate(wavePrefab, canvasObject.transform, false);
            else
                Debug.LogError("HearSaySoundWaves prefab missing from Resources.");
            soundDirectionText = CreateText(canvasObject.transform, "Sound Direction", 28, TextAnchor.MiddleCenter);
            soundDirectionText.raycastTarget = false;
            soundDirectionText.rectTransform.sizeDelta = new Vector2(230f, 60f);
            SetSoundCueVisible(false);

            aslPanel = CreatePanel(canvasObject.transform, "ASL Panel", HearSayTheme.Panel);
            var aslRect = aslPanel.rectTransform;
            aslRect.anchorMin = new Vector2(0.5f, 0f);
            aslRect.anchorMax = new Vector2(0.5f, 0f);
            aslRect.pivot = new Vector2(0.5f, 0f);
            aslRect.sizeDelta = new Vector2(780f, 175f);
            aslRect.anchoredPosition = new Vector2(0f, 52f);
            aslText = CreateText(aslPanel.transform, "ASL detected", 42, TextAnchor.MiddleCenter);
            Stretch(aslText.rectTransform, 18f);

            calibrationText = CreateText(canvasObject.transform, "Speaker Calibration", 26, TextAnchor.UpperLeft);
            RectTransform calibrationRect = calibrationText.rectTransform;
            calibrationRect.anchorMin = new Vector2(0f, 1f);
            calibrationRect.anchorMax = new Vector2(0f, 1f);
            calibrationRect.pivot = new Vector2(0f, 1f);
            calibrationRect.anchoredPosition = new Vector2(28f, -28f);
            calibrationRect.sizeDelta = new Vector2(430f, 135f);

            // Start empty; only explicit server data (or enabled fallback) drives the UI.
            nextRandomStateAt = Time.unscaledTime + 4f;
        }

        private void SetSoundCueVisible(bool visible)
        {
            string direction = (status.soundDirection ?? "").Trim().ToLowerInvariant();
            visible &= direction == "left" || direction == "right";
            bool right = direction == "right";
            if (soundWaves != null)
            {
                // Keep cues clear of the phone's cutout/navigation area.
                Rect safe = UnityEngine.Screen.safeArea;
                float width = Mathf.Max(1, UnityEngine.Screen.width);
                float height = Mathf.Max(1, UnityEngine.Screen.height);
                soundWaves.rectTransform.anchorMin = new Vector2(safe.xMin / width, safe.yMin / height);
                soundWaves.rectTransform.anchorMax = new Vector2(safe.xMax / width, safe.yMax / height);
                soundWaves.Show(visible, right);
            }
            soundDirectionText.gameObject.SetActive(visible);
            if (!visible) return;
            soundDirectionText.text = right ? "SOUND RIGHT" : "SOUND LEFT";
            var rect = soundDirectionText.rectTransform;
            var edge = soundWaves != null
                ? (right ? soundWaves.rectTransform.anchorMax.x : soundWaves.rectTransform.anchorMin.x)
                : (right ? 1f : 0f);
            rect.anchorMin = rect.anchorMax = new Vector2(edge, 0.5f);
            rect.pivot = new Vector2(right ? 1f : 0f, 0.5f);
            rect.anchoredPosition = new Vector2(right ? -20f : 20f, -240f);
        }

        private void SetAslVisible(bool visible)
        {
            aslPanel.gameObject.SetActive(visible);
            if (visible)
            {
                aslText.text = "ASL DETECTED\n" + FormatWord(status.aslWord) + "  (" + Mathf.RoundToInt(status.aslConfidence * 100f) + "%)";
            }
        }

        private void UpdateCalibrationReadout()
        {
            if (calibrationText == null)
            {
                return;
            }

            calibrationText.gameObject.SetActive(showDiagnostics);
            calibrationText.text = serverStatus + "\n" +
                "Mouth: " + SpeakerActivity.CurrentMouthMovement.ToString("F3") + " / 0.070\n" +
                "Mic voice: " + (HearSayAudioActivity.IsVoiceDetected ? "YES" : "NO") + "\n" +
                "Speaker: " + (SpeakerActivity.IsSpeaking ? "CONFIRMED" : "NO SPEAKER DETECTED");
        }

        private void OnDestroy()
        {
            if (uiRoot != null) Destroy(uiRoot);
        }

        private static Image CreatePanel(Transform parent, string name, Color color)
        {
            var panel = new GameObject(name).AddComponent<Image>();
            panel.transform.SetParent(parent, false);
            panel.color = color;
            panel.sprite = HearSayTheme.RoundedSprite;
            panel.type = Image.Type.Sliced;
            panel.raycastTarget = false;
            return panel;
        }

        private static Text CreateText(Transform parent, string name, int fontSize, TextAnchor alignment)
        {
            var text = new GameObject(name).AddComponent<Text>();
            text.transform.SetParent(parent, false);
            text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            text.fontSize = fontSize;
            text.fontStyle = FontStyle.Bold;
            text.alignment = alignment;
            text.color = Color.white;
            text.horizontalOverflow = HorizontalWrapMode.Wrap;
            text.verticalOverflow = VerticalWrapMode.Overflow;
            return text;
        }

        private static void Stretch(RectTransform rect, float padding)
        {
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = new Vector2(padding, padding);
            rect.offsetMax = new Vector2(-padding, -padding);
        }

        private static string FormatWord(string value)
        {
            return string.IsNullOrEmpty(value) ? "UNKNOWN" : value.Replace("_", " ").ToUpperInvariant();
        }
    }
}
