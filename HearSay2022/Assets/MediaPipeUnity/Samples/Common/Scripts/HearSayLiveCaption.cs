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

        /// The separate "HearSay Whisper" app (HearSay > VR > Build and run Whisper APK):
        /// live Whisper on the phone, no Ultra96 needed.
        public static bool IsWhisperPhoneApp => Application.identifier.EndsWith(".whisper");
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

            // "HearSay Demo Sherpa" scene: streaming sherpa-onnx on the phone instead of Whisper.
            if (HearSaySherpa.SceneActive && HearSaySherpa.RunOnPhone)
            {
                StartCoroutine(StartSherpaCaptions());
                isStartingWhisper = false;
                return;
            }

            // 32-bit (ARMv7) phones have no on-phone Whisper library, so they always
            // use the Ultra96 server.
            if ((useUltra96Whisper && !IsWhisperPhoneApp) || IntPtr.Size == 4)
            {
                StartRemoteCaptions();
                isStartingWhisper = false;
                return;
            }

            try
            {
                // The model is loaded only for on-phone captions (initOnAwake is off in the
                // scene), so phones that cannot run Whisper never load it in Ultra96 mode.
                if (IsWhisperPhoneApp)
                {
                    // Live: re-read the last few seconds every second while someone speaks,
                    // so the caption follows the speech about 1 s behind.
                    whisper.stepSec = 1f;
                    whisper.keepSec = 0.2f;
                    whisper.lengthSec = 8f;
                    whisper.useVad = true; // skip silence, so noise doesn't become text
                    Debug.Log("HearSay captions: live Whisper on the phone (tiny.en, 1 s steps)");
                }
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
            if (microphoneRecord != null) microphoneRecord.OnChunkReady -= OnSherpaChunk;
            sherpa?.Dispose();
            sherpa = null;
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

        // ---------------- sherpa-onnx on the phone ("HearSay Demo Sherpa") ----------------
        // Streaming recognizer: every microphone chunk is decoded straight away, so the
        // caption grows word by word while the person is speaking.

        private HearSaySherpa sherpa;
        private bool combined; // sherpa live on the phone + Whisper correction from the Ultra96
        private bool sherpaHeardWords; // combined: sherpa recognised words since the last clip
        private float gateOpenUntil = -1f;  // noise gate stays open until this time
        private float[] gatePrevious;       // last chunk, replayed when speech starts
        private float micLevel, micPeak;    // for the on-screen mic meter (sherpa scene)
        private float micPeakTime;
        private float logMin = 1f, logMax, logSince; // once-a-second mic log (adb logcat)

        private IEnumerator StartSherpaCaptions()
        {
            captionText.text = "Loading speech model…";
            HearSaySherpa created = null;
            string error = null;
            yield return HearSaySherpa.Create((s, e) => { created = s; error = e; });
            if (this == null) { created?.Dispose(); yield break; }
            if (created == null)
            {
                captionText.text = "SHERPA-ONNX ERROR\n" + error;
                Debug.LogError("HearSay captions: " + error);
                yield break;
            }
            sherpa = created;
            microphoneRecord.OnChunkReady -= OnSherpaChunk;
            microphoneRecord.OnChunkReady += OnSherpaChunk;
            microphoneRecord.StartRecord();
            MicrophoneRecording = microphoneRecord.IsRecording;
            captionText.text = "Listening…";
            Debug.Log("HearSay captions: using sherpa-onnx on the phone (streaming)");

            // Two-pass: the same microphone audio is also cut into sentences and sent to
            // Whisper on the Ultra96; its answer replaces sherpa's text for that sentence.
            if (HearSaySherpa.CorrectWithUltra96)
            {
                if (ultra96 == null) ultra96 = FindFirstObjectByType<HearSaySshMqttManager>();
                combined = true;
                microphoneRecord.OnChunkReady -= OnRemoteChunk;
                microphoneRecord.OnChunkReady += OnRemoteChunk;
                Debug.Log("HearSay captions: Whisper on the Ultra96 corrects finished sentences");
            }
        }

        private void OnSherpaChunk(AudioChunk chunk)
        {
            if (sherpa == null) return;
            // Noise gate: speech louder than NoiseGate opens it, and it stays open for
            // GateHoldSeconds so soft syllables and word endings get through. While closed,
            // sherpa gets silence (it still sees the pause that ends a sentence) instead of
            // room noise. The chunk just before speech is replayed so the first sound isn't lost.
            float[] data = chunk.Data;
            if (HearSaySherpa.NoiseGate > 0f && data.Length > 0)
            {
                double sum = 0;
                for (int i = 0; i < data.Length; i++) sum += data[i] * data[i];
                float rms = (float)Math.Sqrt(sum / data.Length);
                micLevel = rms;
                if (rms > micPeak || Time.unscaledTime - micPeakTime > 2f) { micPeak = rms; micPeakTime = Time.unscaledTime; }
                // Every second: quietest and loudest level -> phone log ("HearSay mic").
                // Quietest while nobody talks = room noise; the gate should sit above it.
                logMin = Math.Min(logMin, rms); logMax = Math.Max(logMax, rms);
                if (Time.unscaledTime - logSince >= 1f)
                {
                    Debug.Log($"HearSay mic: lowest {logMin:0.000}  highest {logMax:0.000}  gate {HearSaySherpa.NoiseGate:0.000}  {(logMax >= HearSaySherpa.NoiseGate ? "SPEECH" : "quiet")}");
                    logMin = 1f; logMax = 0f; logSince = Time.unscaledTime;
                }
                bool loud = rms >= HearSaySherpa.NoiseGate;
                bool wasOpen = Time.unscaledTime < gateOpenUntil;
                if (loud) gateOpenUntil = Time.unscaledTime + HearSaySherpa.GateHoldSeconds;
                if (loud && !wasOpen && gatePrevious != null)
                    sherpa.Accept(chunk.Frequency, gatePrevious, out _, out _); // start of the word
                gatePrevious = chunk.Data;
                if (!(loud || wasOpen)) data = new float[data.Length];
            }
            bool changed = sherpa.Accept(chunk.Frequency, data, out string text, out bool finished);
            if (text.Length > 0) sherpaHeardWords = true;
            if ((changed || finished) && text.Length > 0) ShowTranscript(HearSaySherpa.Fix(SentenceCase(text)));
            // Finished sentence -> phone log, so it can be read on the Mac with adb logcat.
            if (finished && text.Length > 0)
                Debug.Log("HearSay caption: " + HearSaySherpa.Fix(SentenceCase(text)));
        }

        // Mic meter (HearSay Sherpa only): live level, the loudest of the last 2 s, and the
        // NoiseGate threshold, so the threshold can be set between room noise and speech.
        private void OnGUI()
        {
            if (sherpa == null || !HearSaySherpa.ShowMicMeter) return;
            float scale = Mathf.Max(1f, Screen.height / 650f);
            var box = new Rect(12 * scale, 12 * scale, 300 * scale, 58 * scale);
            var old = GUI.color;
            GUI.color = new Color(0f, 0f, 0f, 0.55f);
            GUI.DrawTexture(box, Texture2D.whiteTexture);
            // Bar: 0 to 0.15 RMS. Green above the threshold, grey below; white line = threshold.
            const float full = 0.15f;
            var bar = new Rect(box.x + 8 * scale, box.y + 32 * scale, box.width - 16 * scale, 16 * scale);
            GUI.color = new Color(1f, 1f, 1f, 0.15f);
            GUI.DrawTexture(bar, Texture2D.whiteTexture);
            GUI.color = micLevel >= HearSaySherpa.NoiseGate ? new Color(0.3f, 0.9f, 0.4f) : new Color(0.6f, 0.6f, 0.6f);
            GUI.DrawTexture(new Rect(bar.x, bar.y, bar.width * Mathf.Clamp01(micLevel / full), bar.height), Texture2D.whiteTexture);
            GUI.color = Color.white;
            GUI.DrawTexture(new Rect(bar.x + bar.width * Mathf.Clamp01(HearSaySherpa.NoiseGate / full) - scale, bar.y - 3 * scale, 2 * scale, bar.height + 6 * scale), Texture2D.whiteTexture);
            GUI.color = old;
            var style = new GUIStyle(GUI.skin.label) { fontSize = Mathf.RoundToInt(15 * scale) };
            style.normal.textColor = Color.white;
            GUI.Label(new Rect(box.x + 8 * scale, box.y + 4 * scale, box.width, 26 * scale),
                $"Mic {micLevel:0.000}   peak {micPeak:0.000}   gate {HearSaySherpa.NoiseGate:0.000}", style);
        }

        // The English model outputs CAPITALS ("HELLO HOW ARE YOU"): show it as a sentence.
        private static string SentenceCase(string text)
        {
            text = (" " + text.ToLowerInvariant() + " ").Replace(" i ", " I ").Replace(" i'", " I'").Trim();
            return text.Length > 0 ? char.ToUpperInvariant(text[0]) + text.Substring(1) : text;
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

        // Quiet / distant speech: Whisper turns near-silent clips into made-up phrases
        // ("Thank you.", "Thanks for watching!"), so clips are cleaned up before sending.
        private const float MinClipRms = 0.008f;    // quieter than this = background noise only: not sent
        private const float QuietClipRms = 0.02f;   // below this, stock Whisper guesses are ignored
        private const float TargetPeak = 0.9f;      // quiet clips are boosted to this peak level...
        private const float MaxGain = 12f;          // ...by at most this much
        private float lastSentClipRms = 1f;
        private static readonly string[] SilenceGuesses =
        {
            "thank you", "thank you.", "thank you!", "thanks.", "thanks for watching", "thanks for watching!",
            "thank you for watching", "thank you for watching.", "you", "you.", "bye.", "bye", "okay.",
        };

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
            float[] samples = clipSamples.ToArray();
            clipSamples.Clear();
            lastPartialSentTime = -999f;
            // Combined mode: a clip in which sherpa heard no words is background noise.
            // Don't send it, or Whisper invents text ("Thank you", "you") for the silence.
            if (combined)
            {
                if (!sherpaHeardWords) return;
                sherpaHeardWords = false;
            }
            // Loudness of the clip: skip pure background noise, boost quiet speech.
            double sum = 0; float peak = 0f;
            foreach (float v in samples) { sum += v * v; float a = Mathf.Abs(v); if (a > peak) peak = a; }
            float rms = (float)Math.Sqrt(sum / samples.Length);
            if (rms < MinClipRms) { Debug.Log($"HearSay captions: clip skipped, only background noise (rms {rms:F4})"); return; }
            if (peak > 0f)
            {
                float gain = Mathf.Min(MaxGain, TargetPeak / peak);
                if (gain > 1f) for (int i = 0; i < samples.Length; i++) samples[i] *= gain;
            }
            lastSentClipRms = rms;
            byte[] wav = EncodeWav(samples, clipFrequency);
            if (requestInFlight) { queuedClip = wav; return; } // keep only the newest waiting clip
            StartCoroutine(PostClip(wav, false));
        }

        private IEnumerator PostClip(byte[] wav, bool partial)
        {
            requestInFlight = true;
            string url = ultra96 != null ? ultra96.WhisperApiUrl : null;
            if (url == null)
            {
                // Combined mode: sherpa's text stays; the board is only a correction.
                if (!combined)
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
                        {
                            string said = reply.text.Trim();
                            if (lastSentClipRms < QuietClipRms &&
                                Array.IndexOf(SilenceGuesses, said.ToLowerInvariant()) >= 0)
                                Debug.Log($"HearSay captions: ignored \"{said}\" from a quiet clip (rms {lastSentClipRms:F4})");
                            else
                            {
                                // Combined: replace sherpa's sentence only if nobody is
                                // talking again yet (otherwise the new live words stay).
                                if (!combined)
                                    ShowTranscript(said, partial);
                                else if (sherpa == null || sherpa.Partial.Length == 0)
                                {
                                    ShowTranscript(HearSaySherpa.Fix(said));
                                    Debug.Log("HearSay captions: Ultra96 Whisper correction: " + said);
                                }
                            }
                        }
                    }
                    else
                    {
                        Debug.LogWarning("HearSay captions: Ultra96 Whisper request failed: " + request.error);
                        if (!hasTranscript && !combined)
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
