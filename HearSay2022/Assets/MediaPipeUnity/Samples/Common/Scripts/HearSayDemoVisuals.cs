using System.Collections;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.UI;

namespace HearSay
{
    // Visual-only demo layer. It does not modify the speaker, segmentation, or Whisper code.
    public class HearSayDemoVisuals : MonoBehaviour
    {
        [Header("Dummy server")]
        [SerializeField] private string serverUrl = "http://192.168.1.2:8080/status";
        [SerializeField] private float pollIntervalSeconds = 0.25f;
        [SerializeField] private bool useRandomFallback = true;

        private Text soundDirectionText;
        private Image soundPanel;
        private Text aslText;
        private Image aslPanel;
        private Text calibrationText;
        private DemoStatus status = new DemoStatus();
        private float nextRandomStateAt;
        private bool serverHasResponded;

        [System.Serializable]
        private class DemoStatus
        {
            public bool soundActive;
            public string soundDirection;
            public bool aslDetected;
            public string aslWord;
            public float aslConfidence;
        }

        private void Start()
        {
            CreateUi();
            StartCoroutine(PollServer());
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
            SetSoundCueVisible(status.soundActive && !speakerDetected);
            SetAslVisible(status.aslDetected);
            UpdateCalibrationReadout();
        }

        private IEnumerator PollServer()
        {
            while (true)
            {
                using (var request = UnityWebRequest.Get(serverUrl))
                {
                    request.timeout = 2;
                    yield return request.SendWebRequest();

                    if (request.result == UnityWebRequest.Result.Success)
                    {
                        var response = JsonUtility.FromJson<DemoStatus>(request.downloadHandler.text);
                        if (response != null)
                        {
                            status = response;
                            serverHasResponded = true;
                        }
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
            var directions = new[] { "left", "right", "front", "back" };
            status.soundActive = Random.value > 0.2f;
            status.soundDirection = directions[Random.Range(0, directions.Length)];
            status.aslDetected = Random.value > 0.35f;
            status.aslWord = words[Random.Range(0, words.Length)];
            status.aslConfidence = Random.Range(0.78f, 0.98f);
        }

        private void CreateUi()
        {
            var canvasObject = new GameObject("HearSay Demo UI");
            var canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 200;
            var scaler = canvasObject.AddComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ScaleWithScreenSize;
            scaler.referenceResolution = new Vector2(1920, 1080);

            soundPanel = CreatePanel(canvasObject.transform, "Sound Direction Panel", new Color(0.02f, 0.26f, 0.42f, 0.72f));
            var soundRect = soundPanel.rectTransform;
            soundRect.anchorMin = new Vector2(0f, 0.25f);
            soundRect.anchorMax = new Vector2(0f, 0.75f);
            soundRect.pivot = new Vector2(0f, 0.5f);
            soundRect.sizeDelta = new Vector2(390f, 0f);
            soundRect.anchoredPosition = Vector2.zero;
            soundDirectionText = CreateText(soundPanel.transform, "Sound Direction", 64, TextAnchor.MiddleCenter);
            Stretch(soundDirectionText.rectTransform, 24f);

            aslPanel = CreatePanel(canvasObject.transform, "ASL Panel", new Color(0.08f, 0.08f, 0.08f, 0.78f));
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

            CreateRandomStatus();
            nextRandomStateAt = Time.unscaledTime + 4f;
        }

        private void SetSoundCueVisible(bool visible)
        {
            soundPanel.gameObject.SetActive(visible);
            if (!visible)
            {
                return;
            }

            var direction = (status.soundDirection ?? "front").ToLowerInvariant();
            switch (direction)
            {
                case "right":
                    MoveSoundPanel(new Vector2(1f, 0.5f), new Vector2(1f, 0.5f));
                    soundDirectionText.text = "SOUND\n(((  >>>\nRIGHT";
                    break;
                case "front":
                    MoveSoundPanel(new Vector2(0.5f, 1f), new Vector2(0.5f, 1f));
                    soundDirectionText.text = "SOUND AHEAD\n^^^\n((( )))";
                    break;
                case "back":
                    MoveSoundPanel(new Vector2(0.5f, 0f), new Vector2(0.5f, 0f));
                    soundDirectionText.text = "SOUND BEHIND\n((( )))\nvvv";
                    break;
                default:
                    MoveSoundPanel(new Vector2(0f, 0.5f), new Vector2(0f, 0.5f));
                    soundDirectionText.text = "SOUND\n<<<  (((\nLEFT";
                    break;
            }

            var pulse = 0.82f + Mathf.PingPong(Time.unscaledTime * 2.4f, 0.18f);
            soundPanel.color = new Color(0.02f, 0.26f, 0.42f, pulse);
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

            calibrationText.text = "SPEAKER CALIBRATION\n" +
                "Mouth: " + SpeakerActivity.CurrentMouthMovement.ToString("F3") + " / 0.070\n" +
                "Mic voice: " + (HearSayAudioActivity.IsVoiceDetected ? "YES" : "NO") + "\n" +
                "Speaker: " + (SpeakerActivity.IsSpeaking ? "CONFIRMED" : "NO SPEAKER DETECTED");
        }

        private void MoveSoundPanel(Vector2 anchor, Vector2 pivot)
        {
            var rect = soundPanel.rectTransform;
            rect.anchorMin = anchor;
            rect.anchorMax = anchor;
            rect.pivot = pivot;
            rect.anchoredPosition = Vector2.zero;
            if (anchor.y == 0.5f)
            {
                rect.sizeDelta = new Vector2(390f, 540f);
            }
            else
            {
                rect.sizeDelta = new Vector2(680f, 230f);
            }
        }

        private static Image CreatePanel(Transform parent, string name, Color color)
        {
            var panel = new GameObject(name).AddComponent<Image>();
            panel.transform.SetParent(parent, false);
            panel.color = color;
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
