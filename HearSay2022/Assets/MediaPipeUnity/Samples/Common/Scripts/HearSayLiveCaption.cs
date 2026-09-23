using UnityEngine;
using UnityEngine.UI;
using UnityEngine.Networking;
using System;
using System.Collections;
using System.Text;
using Whisper;
using Whisper.Utils;
using System.Threading;

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
        [SerializeField] private bool followActiveSpeaker;
        [SerializeField] private bool hideCaptionWhenSilent;
        [SerializeField] private float captionHideDelaySeconds = 4f;
        [SerializeField] private float bubbleFollowSpeed = 14f;
        [SerializeField] private float bubbleSpeakerLostDelay = 0.75f;
        [SerializeField] private bool showCaptionsOnlyForSpeaker;
        [Header("Caption display")]
        [SerializeField] private int maximumCaptionCharacters = 76;
        [SerializeField] private Vector2 fixedBubbleSize = new Vector2(500f, 145f);

        private WhisperStream stream;
        private GameObject bubble;
        private RectTransform bubbleRect;
        private Text captionText;
        private Text bubbleTail;
        private Image microphoneStatusDot;
        private bool microphonePermissionReady;
        private bool isStartingWhisper;
        private float lastSpeakerDetectedTime;
        private float lastCaptionUpdateTime;
        private bool hasTranscript;
        private float lastFaceBoundsTime;
        private bool hasBubbleTarget;
        private Mediapipe.Unity.Screen cameraPreviewScreen;
        private string pendingCaptionForTerminal;
        private string lastCaptionSentToTerminal;

        private void Start()
        {
            cameraPreviewScreen = FindFirstObjectByType<Mediapipe.Unity.Screen>();
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
                if (this == null || microphoneRecord == null) return;
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
            if (microphoneRecord != null && microphoneRecord.IsRecording)
                microphoneRecord.StopRecord();
            HearSayAudioActivity.SetVoiceDetected(false);
        }

        private void Update()
        {
            if (microphoneStatusDot != null && microphoneRecord != null)
            {
                microphoneStatusDot.color = microphoneRecord.IsVoiceDetected ? Color.green : Color.red;
                HearSayAudioActivity.SetVoiceDetected(microphoneRecord.IsVoiceDetected);
            }

            // Make the bubble visible at the selected person's mouth before
            // Whisper has completed its first transcription segment.
            if (bubble != null && !bubble.activeSelf && SpeakerActivity.IsSpeaking && SpeakerActivity.HasMouthPosition)
            {
                captionText.text = "Listening…";
                bubble.SetActive(true);
                hasBubbleTarget = false;
            }

            if (followActiveSpeaker && bubble != null && bubble.activeSelf && SpeakerActivity.HasMouthPosition)
            {
                lastFaceBoundsTime = Time.unscaledTime;
            }
            else if (followActiveSpeaker && bubble != null && bubble.activeSelf &&
                     Time.unscaledTime - lastFaceBoundsTime >= bubbleSpeakerLostDelay)
            {
                bubble.SetActive(false);
                hasBubbleTarget = false;
            }

            if (hideCaptionWhenSilent && hasTranscript && bubble != null && bubble.activeSelf &&
                Time.unscaledTime - lastCaptionUpdateTime >= captionHideDelaySeconds)
            {
                bubble.SetActive(false);
                hasBubbleTarget = false;
            }

            if (!string.IsNullOrEmpty(pendingCaptionForTerminal) &&
                pendingCaptionForTerminal != lastCaptionSentToTerminal)
            {
                lastCaptionSentToTerminal = pendingCaptionForTerminal;
                StartCoroutine(SendCaptionToTerminal(pendingCaptionForTerminal));
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

            string cleaned = GetNewestCaption(transcript);
            if (showCaptionsOnlyForSpeaker && !SpeakerActivity.IsSpeaking)
            {
                return;
            }
            captionText.text = cleaned;
            pendingCaptionForTerminal = cleaned;
            bubble.SetActive(cleaned.Length > 0);
            hasTranscript = cleaned.Length > 0;
            if (hasTranscript)
            {
                lastCaptionUpdateTime = Time.unscaledTime;
            }
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
            background.color = HearSayTheme.Panel;
            background.sprite = HearSayTheme.RoundedSprite;
            background.type = Image.Type.Sliced;
            background.raycastTarget = false;

            bubbleRect = bubble.GetComponent<RectTransform>();
            bubbleRect.anchorMin = new Vector2(0.5f, 0f);
            bubbleRect.anchorMax = new Vector2(0.5f, 0f);
            bubbleRect.pivot = new Vector2(0.5f, 0f);
            bubbleRect.anchoredPosition = new Vector2(0f, 110f);
            bubbleRect.sizeDelta = fixedBubbleSize;

            GameObject tailObject = new GameObject("Speech Bubble Tail");
            tailObject.transform.SetParent(bubble.transform, false);
            bubbleTail = tailObject.AddComponent<Text>();
            bubbleTail.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            bubbleTail.fontSize = 52;
            bubbleTail.alignment = TextAnchor.MiddleCenter;
            bubbleTail.color = background.color;

            GameObject textObject = new GameObject("Transcript Text");
            textObject.transform.SetParent(bubble.transform, false);
            captionText = textObject.AddComponent<Text>();
            captionText.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            captionText.fontSize = 34;
            captionText.alignment = TextAnchor.MiddleLeft;
            captionText.horizontalOverflow = HorizontalWrapMode.Wrap;
            captionText.verticalOverflow = VerticalWrapMode.Truncate;
            captionText.color = Color.white;

            RectTransform textRect = captionText.rectTransform;
            textRect.anchorMin = Vector2.zero;
            textRect.anchorMax = Vector2.one;
            textRect.offsetMin = new Vector2(28f, 18f);
            textRect.offsetMax = new Vector2(-28f, -18f);

            bubble.SetActive(false);
        }

        private void FollowActiveSpeaker()
        {
            Vector2 mouthScreenPosition = ToScreenPoint(SpeakerActivity.CurrentMouthPosition);
            bool placeToRight = mouthScreenPosition.x < Screen.width * 0.55f;

            bubbleRect.anchorMin = new Vector2(0.5f, 0.5f);
            bubbleRect.anchorMax = new Vector2(0.5f, 0.5f);
            bubbleRect.pivot = placeToRight ? new Vector2(0f, 0.5f) : new Vector2(1f, 0.5f);
            bubbleRect.sizeDelta = fixedBubbleSize;
            var parent = (RectTransform)bubbleRect.parent;
            RectTransformUtility.ScreenPointToLocalPointInRectangle(parent, mouthScreenPosition, null, out var mouthLocal);
            // Keep the bubble at lip height with a small gap for its tail.
            bubbleRect.anchoredPosition = mouthLocal + new Vector2(placeToRight ? 48f : -48f, 0f);
            hasBubbleTarget = true;

            bubbleTail.text = placeToRight ? "◀" : "▶";
            RectTransform tailRect = bubbleTail.rectTransform;
            tailRect.anchorMin = new Vector2(placeToRight ? 0f : 1f, 0.5f);
            tailRect.anchorMax = tailRect.anchorMin;
            tailRect.pivot = new Vector2(placeToRight ? 1f : 0f, 0.5f);
            tailRect.anchoredPosition = Vector2.zero;
            tailRect.sizeDelta = new Vector2(48f, 80f);
        }

        private void LateUpdate()
        {
            // Read the landmarks after SpeakerMouthTest.Update has published them.
            if (followActiveSpeaker && bubble != null && bubble.activeSelf && SpeakerActivity.HasMouthPosition)
                FollowActiveSpeaker();
        }

        private Vector2 ToScreenPoint(Vector2 normalizedPoint)
        {
            if (cameraPreviewScreen == null)
            {
                cameraPreviewScreen = FindFirstObjectByType<Mediapipe.Unity.Screen>();
            }

            if (cameraPreviewScreen != null)
            {
                return cameraPreviewScreen.NormalizedLandmarkToScreenPoint(normalizedPoint);
            }

            return new Vector2(normalizedPoint.x * Screen.width, (1f - normalizedPoint.y) * Screen.height);
        }

        private string GetNewestCaption(string transcript)
        {
            string cleaned = transcript.Trim();
            if (cleaned.Length == 0)
            {
                return string.Empty;
            }

            // Whisper can return its accumulated transcript. Keep the newest
            // sentence/line so earlier spoken text does not remain in the bubble.
            int lastNewLine = cleaned.LastIndexOf('\n');
            if (lastNewLine >= 0 && lastNewLine < cleaned.Length - 1)
            {
                cleaned = cleaned.Substring(lastNewLine + 1).Trim();
            }
            else if (cleaned.Length > 1)
            {
                int previousSentenceEnd = cleaned.LastIndexOfAny(
                    new[] { '.', '!', '?' }, cleaned.Length - 2);
                if (previousSentenceEnd >= 0)
                {
                    cleaned = cleaned.Substring(previousSentenceEnd + 1).Trim();
                }
            }

            int maxLength = Mathf.Max(1, maximumCaptionCharacters);
            if (cleaned.Length > maxLength)
            {
                cleaned = "…" + cleaned.Substring(cleaned.Length - maxLength + 1);
            }
            return cleaned;
        }

        private IEnumerator SendCaptionToTerminal(string caption)
        {
            string statusUrl = HearSayDemoVisuals.ActiveServerUrl;
            if (string.IsNullOrEmpty(statusUrl))
            {
                yield break;
            }

            string captionUrl = statusUrl.Replace("/status", "/caption");
            byte[] body = Encoding.UTF8.GetBytes(JsonUtility.ToJson(new CaptionPayload { caption = caption }));
            using (var request = new UnityWebRequest(captionUrl, UnityWebRequest.kHttpVerbPOST))
            {
                request.uploadHandler = new UploadHandlerRaw(body);
                request.downloadHandler = new DownloadHandlerBuffer();
                request.SetRequestHeader("Content-Type", "application/json");
                yield return request.SendWebRequest();
                if (request.result != UnityWebRequest.Result.Success)
                {
                    Debug.LogWarning("HearSay caption debug server unavailable: " + request.error);
                }
            }
        }

        [Serializable]
        private class CaptionPayload
        {
            public string caption;
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

    // Shared microphone activity signal for the speaker gate. Uses no Unity API so it is
    // safe to read from the MediaPipe result callback.
    public static class HearSayAudioActivity
    {
        private static int lastVoiceDetectedTick;
        private static int isVoiceDetected;
        public static bool IsVoiceDetected => Volatile.Read(ref isVoiceDetected) != 0;

        public static void SetVoiceDetected(bool isVoiceDetected)
        {
            Volatile.Write(ref HearSayAudioActivity.isVoiceDetected, isVoiceDetected ? 1 : 0);
            if (isVoiceDetected)
            {
                Interlocked.Exchange(ref lastVoiceDetectedTick, System.Environment.TickCount);
            }
        }

        public static bool VoiceDetectedRecently(float holdSeconds)
        {
            if (IsVoiceDetected)
            {
                return true;
            }

            int elapsed = unchecked(System.Environment.TickCount - Volatile.Read(ref lastVoiceDetectedTick));
            return elapsed >= 0 && elapsed <= Mathf.RoundToInt(holdSeconds * 1000f);
        }
    }
}
