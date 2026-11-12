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
        public static string LatestCaption { get; private set; } = "";
        public static bool MicrophoneRecording { get; private set; }
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
        [Header("Speech recognition on the Ultra96")]
        [Tooltip("On: send speech clips to the Whisper API on the Ultra96 (whisper_local/run_server.sh) " +
                 "through the SSH tunnel; the phone never loads Whisper itself, so phones that can't run it work. " +
                 "Off: run Whisper on the phone as before.")]
        [SerializeField] private bool useUltra96Whisper = true;
        [Tooltip("Connection to the Ultra96 (found automatically if empty).")]
        [SerializeField] private HearSaySshMqttManager ultra96;
        [Tooltip("A clip is sent when speech pauses for this long, or reaches the maximum length.")]
        [SerializeField] private float pauseToSendSeconds = 0.5f;
        [Tooltip("Keep at or below what run_server.sh's -ac allows (-ac 384 = 7.68 s, minus one chunk).")]
        [SerializeField] private float maxClipSeconds = 6f;
        [Tooltip("While someone is still talking, send the speech so far this often and show it as a " +
                 "partial caption. Skipped while a request is in flight, so a slow Ultra96 just gets fewer. 0 = off.")]
        [SerializeField] private float partialEverySeconds = 1f;

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
        private bool bubbleOnRight;
        private Mediapipe.Unity.Screen cameraPreviewScreen;
        private string pendingCaptionForTerminal;
        private string lastCaptionSentToTerminal;

        private void Awake()
        {
            if (UnityEngine.SceneManagement.SceneManager.GetActiveScene().name == "HearSay Demo" &&
                PlayerPrefs.GetInt("HearSay.VrRole", 0) != 1)
            {
                // Do not even allow the Deaf-user Whisper UI to initialize in
                // Conversation partner mode.
                enabled = false;
            }
        }

        private void Start()
        {
            // Live Whisper captions belong only to the Deaf-user role. The
            // Conversation partner role has its own ASL translation UI and
            // must not show the Deaf-user "Waiting for speaker" bubble.
            if (UnityEngine.SceneManagement.SceneManager.GetActiveScene().name == "HearSay Demo" &&
                PlayerPrefs.GetInt("HearSay.VrRole", 0) != 1)
            {
                Debug.Log("HearSay captions: disabled for Conversation partner role");
                enabled = false;
                return;
            }
            LatestCaption = "";
            MicrophoneRecording = false;
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

            // 32-bit (ARMv7) phones have no on-phone Whisper library, so they always
            // use the Ultra96 server.
            if (useUltra96Whisper || IntPtr.Size == 4)
            {
                StartRemoteCaptions();
                isStartingWhisper = false;
                return;
            }

            try
            {
                // The model is loaded only for on-phone captions (initOnAwake is off in the
                // scene), so phones that cannot run Whisper never load it in Ultra96 mode.
                if (!whisper.IsLoaded && !whisper.IsLoading) await whisper.InitModel();
                stream = await whisper.CreateStream(microphoneRecord);
                if (this == null || microphoneRecord == null) return;
                stream.OnResultUpdated += ShowTranscript;

                stream.StartStream();
                microphoneRecord.StartRecord();
                MicrophoneRecording = microphoneRecord.IsRecording;
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
            if (microphoneRecord != null) microphoneRecord.OnChunkReady -= OnRemoteChunk;
            if (stream != null)
            {
                stream.OnResultUpdated -= ShowTranscript;
            }
            if (microphoneRecord != null && microphoneRecord.IsRecording)
                microphoneRecord.StopRecord();
            MicrophoneRecording = false;
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
                MicrophoneRecording = false;
                captionText.text = "Waiting for speaker…";
            }
        }

        private void ShowTranscript(string transcript) => ShowTranscript(transcript, false);

        // partial: text for speech that is still going on. It is shown with "…" and
        // replaced by the final text, and isn't sent to the terminal.
        private void ShowTranscript(string transcript, bool partial)
        {
            if (captionText == null) return;

            string cleaned = GetNewestCaption(RemoveNonSpeechTags(transcript));
            // Whisper labels unclear audio as "[Music]", "(wind)", "[BLANK_AUDIO]" etc.
            // Keep the last real caption instead of showing those labels.
            if (cleaned.Length == 0 && transcript.Trim().Length > 0) return;
            if (partial && cleaned.Length == 0) return;
            if (!partial) LatestCaption = cleaned;
            if (showCaptionsOnlyForSpeaker && !SpeakerActivity.IsSpeaking)
            {
                return;
            }
            captionText.text = partial ? cleaned + " …" : cleaned;
            if (!partial) pendingCaptionForTerminal = cleaned;
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
            // Hysteresis prevents the bubble flipping sides near the screen centre.
            bool placeToRight = !hasBubbleTarget
                ? mouthScreenPosition.x < Screen.width * 0.5f
                : (bubbleOnRight ? mouthScreenPosition.x < Screen.width * 0.62f
                                 : mouthScreenPosition.x < Screen.width * 0.38f);
            bool sideChanged = hasBubbleTarget && placeToRight != bubbleOnRight;
            bubbleOnRight = placeToRight;

            bubbleRect.anchorMin = new Vector2(0.5f, 0.5f);
            bubbleRect.anchorMax = new Vector2(0.5f, 0.5f);
            bubbleRect.pivot = placeToRight ? new Vector2(0f, 0.5f) : new Vector2(1f, 0.5f);
            bubbleRect.sizeDelta = fixedBubbleSize;
            var parent = (RectTransform)bubbleRect.parent;
            RectTransformUtility.ScreenPointToLocalPointInRectangle(parent, mouthScreenPosition, null, out var mouthLocal);
            // Keep the bubble at lip height with a small gap for its tail.
            Vector2 target = mouthLocal + new Vector2(placeToRight ? 48f : -48f, 0f);
            Rect safe = Screen.safeArea;
            RectTransformUtility.ScreenPointToLocalPointInRectangle(parent, safe.min, null, out var safeMin);
            RectTransformUtility.ScreenPointToLocalPointInRectangle(parent, safe.max, null, out var safeMax);
            Vector2 size = new Vector2(Mathf.Min(fixedBubbleSize.x, safeMax.x - safeMin.x - 32f),
                Mathf.Min(fixedBubbleSize.y, safeMax.y - safeMin.y - 32f));
            bubbleRect.sizeDelta = size;
            target.x = Mathf.Clamp(target.x, safeMin.x + 16f + size.x * bubbleRect.pivot.x,
                safeMax.x - 16f - size.x * (1f - bubbleRect.pivot.x));
            target.y = Mathf.Clamp(target.y, safeMin.y + 16f + size.y * 0.5f,
                safeMax.y - 16f - size.y * 0.5f);
            // Smooth only small tracking noise; snap after a large jump or a side switch.
            float distance = Vector2.Distance(bubbleRect.anchoredPosition, target);
            float blend = 1f - Mathf.Exp(-Mathf.Max(18f, bubbleFollowSpeed) * Time.unscaledDeltaTime);
            if (!hasBubbleTarget || sideChanged || distance > 180f)
                bubbleRect.anchoredPosition = target;
            else if (distance > 2f)
                bubbleRect.anchoredPosition = Vector2.Lerp(bubbleRect.anchoredPosition, target, blend);
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
            else
                hasBubbleTarget = false;
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

        private static string RemoveNonSpeechTags(string transcript)
        {
            if (string.IsNullOrEmpty(transcript)) return string.Empty;
            string text = System.Text.RegularExpressions.Regex.Replace(transcript, @"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|[♪♫]", " ");
            return System.Text.RegularExpressions.Regex.Replace(text, @"[ \t]+", " ").Trim();
        }

        // ---------------- Ultra96 Whisper API ----------------
        // The phone records with the existing microphone + voice detection, cuts the
        // speech into short clips (at a pause or after maxClipSeconds), sends each clip
        // as a WAV to the Ultra96's whisper-server and shows the returned text.

        private readonly System.Collections.Generic.List<float> clipSamples = new System.Collections.Generic.List<float>();
        private float[] previousChunk;          // a little audio from before speech started
        private float lastVoiceTime = -999f;
        private int clipFrequency = 16000;
        private bool requestInFlight;
        private byte[] queuedClip;              // a finished clip waiting for the request in flight
        private float lastPartialSentTime = -999f;

        [Serializable] private class WhisperApiReply { public string text; public string error; }

        private void StartRemoteCaptions()
        {
            if (ultra96 == null) ultra96 = FindFirstObjectByType<HearSaySshMqttManager>();
            microphoneRecord.OnChunkReady -= OnRemoteChunk;
            microphoneRecord.OnChunkReady += OnRemoteChunk;
            microphoneRecord.StartRecord();
            captionText.text = ultra96 != null && ultra96.IsConnected ? "Listening…" : "Connecting to Ultra96…";
            Debug.Log("HearSay captions: using Whisper on the Ultra96 (" + (ultra96 != null ? ultra96.ConnectionStatus : "no SSH manager in scene") + ")");
        }

        private void OnRemoteChunk(AudioChunk chunk)
        {
            clipFrequency = chunk.Frequency;
            bool voice = chunk.IsVoiceDetected;
            if (voice) lastVoiceTime = Time.unscaledTime;

            bool inSpeech = Time.unscaledTime - lastVoiceTime < pauseToSendSeconds;
            if (inSpeech)
            {
                if (clipSamples.Count == 0 && previousChunk != null) clipSamples.AddRange(previousChunk);
                clipSamples.AddRange(chunk.Data);
            }
            previousChunk = chunk.Data;

            float seconds = clipSamples.Count / (float)Mathf.Max(1, clipFrequency);
            if ((!inSpeech && seconds >= 0.5f) || seconds >= maxClipSeconds)
                SendClip();
            else if (!inSpeech)
                clipSamples.Clear();
            else if (partialEverySeconds > 0f && seconds >= 1f && !requestInFlight &&
                     Time.unscaledTime - lastPartialSentTime >= partialEverySeconds)
                SendPartial();
        }

        // The speech so far, sent without ending the clip, for a caption that updates while talking.
        // Only sent when nothing is in flight, so partials never delay a finished clip.
        private void SendPartial()
        {
            lastPartialSentTime = Time.unscaledTime;
            StartCoroutine(PostClip(EncodeWav(clipSamples.ToArray(), clipFrequency), true));
        }

        private void SendClip()
        {
            if (clipSamples.Count == 0) return;
            byte[] wav = EncodeWav(clipSamples.ToArray(), clipFrequency);
            clipSamples.Clear();
            lastPartialSentTime = -999f;
            if (requestInFlight) { queuedClip = wav; return; } // keep only the newest waiting clip
            StartCoroutine(PostClip(wav, false));
        }

        private IEnumerator PostClip(byte[] wav, bool partial)
        {
            requestInFlight = true;
            string url = ultra96 != null ? ultra96.WhisperApiUrl : null;
            if (url == null)
            {
                captionText.text = "Waiting for Ultra96 connection…\n" + (ultra96 != null ? ultra96.ConnectionStatus : "");
            }
            else
            {
                var form = new System.Collections.Generic.List<IMultipartFormSection>
                {
                    new MultipartFormFileSection("file", wav, "speech.wav", "audio/wav"),
                    new MultipartFormDataSection("response_format", "json"),
                    new MultipartFormDataSection("temperature", "0"),
                };
                using (var request = UnityWebRequest.Post(url, form))
                {
                    request.timeout = 30;
                    yield return request.SendWebRequest();
                    if (request.result == UnityWebRequest.Result.Success)
                    {
                        WhisperApiReply reply = null;
                        try { reply = JsonUtility.FromJson<WhisperApiReply>(request.downloadHandler.text); }
                        catch (Exception e) { Debug.LogWarning("HearSay captions: bad reply from Ultra96: " + e.Message); }
                        if (reply != null && !string.IsNullOrEmpty(reply.error))
                            Debug.LogWarning("HearSay captions: Ultra96 Whisper error: " + reply.error);
                        else if (reply != null && !string.IsNullOrEmpty(reply.text))
                            ShowTranscript(reply.text.Trim(), partial);
                    }
                    else
                    {
                        Debug.LogWarning("HearSay captions: Ultra96 Whisper request failed: " + request.error);
                        if (!hasTranscript)
                            captionText.text = "Ultra96 Whisper not reachable.\nIs run_server.sh running on the Ultra96?";
                    }
                }
            }
            requestInFlight = false;
            if (queuedClip != null)
            {
                byte[] next = queuedClip;
                queuedClip = null;
                StartCoroutine(PostClip(next, false));
            }
        }

        // 16-bit PCM mono WAV, the format whisper-server expects (16 kHz).
        private static byte[] EncodeWav(float[] samples, int frequency)
        {
            int dataBytes = samples.Length * 2;
            var bytes = new byte[44 + dataBytes];
            void Str(int at, string s) { for (int i = 0; i < 4; i++) bytes[at + i] = (byte)s[i]; }
            void Int(int at, int v) { BitConverter.GetBytes(v).CopyTo(bytes, at); }
            void Short(int at, short v) { BitConverter.GetBytes(v).CopyTo(bytes, at); }
            Str(0, "RIFF"); Int(4, 36 + dataBytes); Str(8, "WAVE");
            Str(12, "fmt "); Int(16, 16); Short(20, 1); Short(22, 1);
            Int(24, frequency); Int(28, frequency * 2); Short(32, 2); Short(34, 16);
            Str(36, "data"); Int(40, dataBytes);
            for (int i = 0; i < samples.Length; i++)
            {
                short s = (short)(Mathf.Clamp(samples[i], -1f, 1f) * short.MaxValue);
                bytes[44 + i * 2] = (byte)(s & 0xff);
                bytes[45 + i * 2] = (byte)((s >> 8) & 0xff);
            }
            return bytes;
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
            if (HearSayLocalMqtt.IsMqtt(statusUrl))
            {
                _ = HearSayLocalMqtt.For(statusUrl).Report("caption", JsonUtility.ToJson(new CaptionPayload { caption = caption }));
                yield break;
            }
            // No HTTP dummy server: captions are only shown on the phone.
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
