using UnityEngine;
using UnityEngine.UI;
using System;
using System.Collections;
using Whisper;
using Whisper.Utils;

namespace HearSay
{
    // Streams local Whisper transcription into a simple on-screen speech bubble.
    public class HearSayLiveCaption : MonoBehaviour
    {
        [SerializeField] private WhisperManager whisper;
        [SerializeField] private MicrophoneRecord microphoneRecord;

        private WhisperStream stream;
        private GameObject bubble;
        private Text captionText;

        private void Start()
        {
            CreateSpeechBubble();
            bubble.SetActive(true);
            StartWhisper();
        }

        private async void StartWhisper()
        {

            if (Microphone.devices == null || Microphone.devices.Length == 0)
            {
                captionText.text = "NO MICROPHONE DETECTED\nEnable Unity microphone access in macOS Settings.";
                Debug.LogError("HearSay captions: Unity found no microphone devices.");
                return;
            }

            captionText.text = "Listening…";
            microphoneRecord.SelectedMicDevice = Microphone.devices[0];
            Debug.Log("HearSay captions: microphone = " + microphoneRecord.SelectedMicDevice);

            try
            {
                stream = await whisper.CreateStream(microphoneRecord);
                stream.OnResultUpdated += ShowTranscript;

                stream.StartStream();
                microphoneRecord.StartRecord();
                Debug.Log("HearSay captions: local Whisper microphone stream started.");
            }
            catch (Exception exception)
            {
                captionText.text = "MICROPHONE / WHISPER ERROR\nCheck the Unity Console.";
                Debug.LogException(exception);
            }
        }

        private void OnDestroy()
        {
            if (stream != null)
            {
                stream.OnResultUpdated -= ShowTranscript;
            }
        }

        private void ShowTranscript(string transcript)
        {
            if (captionText == null) return;

            string cleaned = transcript.Trim();
            captionText.text = cleaned;
            bubble.SetActive(cleaned.Length > 0);
        }

        private void CreateSpeechBubble()
        {
            GameObject canvasObject = new GameObject("HearSay Captions");
            Canvas canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 210;
            canvasObject.AddComponent<CanvasScaler>();
            canvasObject.AddComponent<GraphicRaycaster>();

            bubble = new GameObject("Live Speech Bubble");
            bubble.transform.SetParent(canvasObject.transform, false);
            Image background = bubble.AddComponent<Image>();
            background.color = new Color(0.05f, 0.05f, 0.05f, 0.82f);

            RectTransform bubbleRect = bubble.GetComponent<RectTransform>();
            bubbleRect.anchorMin = new Vector2(0.5f, 0f);
            bubbleRect.anchorMax = new Vector2(0.5f, 0f);
            bubbleRect.pivot = new Vector2(0.5f, 0f);
            bubbleRect.anchoredPosition = new Vector2(0f, 110f);
            bubbleRect.sizeDelta = new Vector2(850f, 180f);

            GameObject textObject = new GameObject("Transcript Text");
            textObject.transform.SetParent(bubble.transform, false);
            captionText = textObject.AddComponent<Text>();
            captionText.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            captionText.fontSize = 34;
            captionText.alignment = TextAnchor.MiddleCenter;
            captionText.horizontalOverflow = HorizontalWrapMode.Wrap;
            captionText.verticalOverflow = VerticalWrapMode.Overflow;
            captionText.color = Color.white;

            RectTransform textRect = captionText.rectTransform;
            textRect.anchorMin = Vector2.zero;
            textRect.anchorMax = Vector2.one;
            textRect.offsetMin = new Vector2(28f, 18f);
            textRect.offsetMax = new Vector2(-28f, -18f);

            bubble.SetActive(false);
        }
    }
}
