using UnityEngine;
using UnityEngine.UI;
using System;
using System.Collections;
using Whisper;
using Whisper.Utils;

#if UNITY_ANDROID && !UNITY_EDITOR
using UnityEngine.Android;
#endif

namespace HearSay
{
    // Streams local Whisper transcription into a simple on-screen speech bubble.
    public class HearSayLiveCaption : MonoBehaviour
    {
        [SerializeField] private WhisperManager whisper;
        [SerializeField] private MicrophoneRecord microphoneRecord;
        [SerializeField] private bool startOnlyWhenSpeakerDetected;
        [SerializeField] private float speakerLostDelay = 1.25f;

        private WhisperStream stream;
        private GameObject bubble;
        private Text captionText;
        private Image microphoneStatusDot;
        private bool microphonePermissionReady;
        private bool isStartingWhisper;
        private float lastSpeakerDetectedTime;

        private void Start()
        {
            CreateSpeechBubble();
            bubble.SetActive(true);
            StartCoroutine(StartAfterMicrophonePermission());
        }

        private IEnumerator StartAfterMicrophonePermission()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!Permission.HasUserAuthorizedPermission(Permission.Microphone))
            {
                captionText.text = "Microphone permission needed…";
                var permissionRequestFinished = false;
                var callbacks = new PermissionCallbacks();
                callbacks.PermissionGranted += _ => permissionRequestFinished = true;
                callbacks.PermissionDenied += _ => permissionRequestFinished = true;
                callbacks.PermissionDeniedAndDontAskAgain += _ => permissionRequestFinished = true;
                Permission.RequestUserPermission(Permission.Microphone, callbacks);
                yield return new WaitUntil(() => permissionRequestFinished);
            }

            if (!Permission.HasUserAuthorizedPermission(Permission.Microphone))
            {
                captionText.text = "MICROPHONE PERMISSION DENIED\nEnable Microphone for HearSay in Android Settings.";
                yield break;
            }
#endif

            microphonePermissionReady = true;
            if (startOnlyWhenSpeakerDetected)
            {
                captionText.text = "Waiting for speaker…";
            }
            else
            {
                StartWhisper();
            }
            yield break;
        }

        private async void StartWhisper()
        {
            if (isStartingWhisper || microphoneRecord.IsRecording)
            {
                return;
            }
            if (Microphone.devices == null || Microphone.devices.Length == 0)
            {
                captionText.text = "NO MICROPHONE DETECTED\nCheck the device microphone permission and input.";
                Debug.LogError("HearSay captions: Unity found no microphone devices.");
                return;
            }

            isStartingWhisper = true;
            captionText.text = startOnlyWhenSpeakerDetected
                ? "Speaker detected — listening…"
                : "Listening…";
#if UNITY_ANDROID && !UNITY_EDITOR
            microphoneRecord.SelectedMicDevice = null;
#else
            microphoneRecord.SelectedMicDevice = Microphone.devices[0];
#endif
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
            finally
            {
                isStartingWhisper = false;
            }
        }

        private void OnDestroy()
        {
            if (stream != null)
            {
                stream.OnResultUpdated -= ShowTranscript;
            }
        }

        private void Update()
        {
            if (microphoneStatusDot != null && microphoneRecord != null)
            {
                microphoneStatusDot.color = microphoneRecord.IsVoiceDetected ? Color.green : Color.red;
            }

            if (!startOnlyWhenSpeakerDetected || !microphonePermissionReady || microphoneRecord == null)
            {
                return;
            }

            if (SpeakerActivity.IsSpeaking)
            {
                lastSpeakerDetectedTime = Time.unscaledTime;
                if (!microphoneRecord.IsRecording)
                {
                    StartWhisper();
                }
            }
            else if (microphoneRecord.IsRecording &&
                     Time.unscaledTime - lastSpeakerDetectedTime >= speakerLostDelay)
            {
                microphoneRecord.StopRecord();
                captionText.text = "Waiting for speaker…";
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

            GameObject statusDotObject = new GameObject("Microphone Status Dot");
            statusDotObject.transform.SetParent(canvasObject.transform, false);
            microphoneStatusDot = statusDotObject.AddComponent<Image>();
            microphoneStatusDot.sprite = CreateCircleSprite();
            microphoneStatusDot.color = Color.red;

            RectTransform statusDotRect = microphoneStatusDot.rectTransform;
            statusDotRect.anchorMin = new Vector2(1f, 1f);
            statusDotRect.anchorMax = new Vector2(1f, 1f);
            statusDotRect.pivot = new Vector2(1f, 1f);
            statusDotRect.anchoredPosition = new Vector2(-36f, -36f);
            statusDotRect.sizeDelta = new Vector2(38f, 38f);

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

        private static Sprite CreateCircleSprite()
        {
            const int size = 64;
            var texture = new Texture2D(size, size, TextureFormat.RGBA32, false);
            var pixels = new Color[size * size];
            var radius = size * 0.5f;
            var center = (size - 1) * 0.5f;

            for (var y = 0; y < size; y++)
            {
                for (var x = 0; x < size; x++)
                {
                    var distance = Vector2.Distance(new Vector2(x, y), new Vector2(center, center));
                    pixels[y * size + x] = new Color(1f, 1f, 1f, distance <= radius ? 1f : 0f);
                }
            }

            texture.SetPixels(pixels);
            texture.Apply();
            return Sprite.Create(texture, new Rect(0f, 0f, size, size), new Vector2(0.5f, 0.5f));
        }
    }
}
