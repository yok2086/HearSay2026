using System;
using UnityEngine;
using Mediapipe.Tasks.Core;
using Mediapipe.Tasks.Vision.Core;
using Mediapipe.Tasks.Vision.HandLandmarker;

namespace HearSay
{
    // Partner-only visual tracker. It never opens a camera or recognises ASL words.
    public class HearSayPartnerHands : MonoBehaviour
    {
        [SerializeField] private TextAsset model;
        [SerializeField, Range(4, 20)] private int trackingFps = 12;
        public string Word { get; set; }
        private HearSayPartnerCamera source;
        private HandLandmarker detector;
        private RenderTexture capture;
        private Texture2D readback, upright;
        private Color32[] orientedPixels;
        private Mediapipe.Image input;
        private HearSayHandGraphic graphic;
        private GameObject overlay;
        private readonly object gate = new object();
        private Vector2[][] pending;
        private Vector2[][] displayed;
        private bool completed, busy;
        private float submittedAt, receivedAt, nextFrame;
        private int imageWidth, imageHeight, submittedAngle;
        private bool submittedMirror;
        private long timestamp;
        private string error;

        public void Initialize(HearSayPartnerCamera cameraSource) { source = cameraSource; }

        private void Start()
        {
            try
            {
                if (model == null) throw new InvalidOperationException("Hand model missing from partner prefab.");
                detector = HandLandmarker.CreateFromOptions(new HandLandmarkerOptions(
                    new BaseOptions(BaseOptions.Delegate.CPU, modelAssetBuffer: model.bytes),
                    runningMode: RunningMode.LIVE_STREAM, numHands: 2,
                    minHandDetectionConfidence: 0.6f, minHandPresenceConfidence: 0.6f,
                    minTrackingConfidence: 0.6f, resultCallback: OnResult));
                overlay = new GameObject("Partner hand landmarks", typeof(Canvas));
                var canvas = overlay.GetComponent<Canvas>();
                canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                canvas.sortingOrder = 50;
                graphic = new GameObject("Cyan hand skeleton", typeof(RectTransform),
                    typeof(UnityEngine.CanvasRenderer), typeof(HearSayHandGraphic)).GetComponent<HearSayHandGraphic>();
                graphic.transform.SetParent(overlay.transform, false);
                graphic.rectTransform.anchorMin = Vector2.zero;
                graphic.rectTransform.anchorMax = Vector2.one;
                graphic.rectTransform.offsetMin = graphic.rectTransform.offsetMax = Vector2.zero;
                graphic.raycastTarget = false;
                graphic.color = HearSayTheme.Accent;
            }
            catch (Exception e) { Fail(e); }
        }

        private void LateUpdate()
        {
            if (source == null || detector == null || error != null) return;
            var camera = source.CurrentTexture;
            lock (gate)
            {
                if (completed)
                {
                    // Callback has finished reading the input. Only now may its buffer be reused.
                    input?.Dispose(); input = null;
                    displayed = pending;
                    pending = null;
                    completed = false;
                    busy = false;
                    receivedAt = Time.unscaledTime;
                }
            }
            bool validOrientation = camera != null && camera.videoRotationAngle == submittedAngle &&
                camera.videoVerticallyMirrored == submittedMirror;
            if (graphic != null)
                graphic.SetHands(validOrientation && Time.unscaledTime - receivedAt < 0.45f ? displayed : null,
                    imageWidth, imageHeight);
            if (busy)
            {
                // Don't reuse a native input buffer if an inference stalls.
                if (Time.unscaledTime - submittedAt > 5f) error = "Hand tracking paused. Reopen this view to retry.";
                return;
            }
            if (camera == null || camera.width <= 16 || !camera.didUpdateThisFrame ||
                Time.unscaledTime < nextFrame) return;
            nextFrame = Time.unscaledTime + 1f / trackingFps;
            try
            {
                PrepareImage(camera);
                timestamp = Math.Max(timestamp + 1, (long)(Time.realtimeSinceStartupAsDouble * 1000));
                input = new Mediapipe.Image(upright);
                busy = true;
                submittedAt = Time.unscaledTime;
                detector.DetectAsync(input, timestamp);
            }
            catch (Exception e) { Fail(e); }
        }

        private void PrepareImage(WebCamTexture camera)
        {
            int width = Mathf.Min(640, camera.width);
            int height = Mathf.Max(1, Mathf.RoundToInt(width * camera.height / (float)camera.width));
            if (capture == null || capture.width != width || capture.height != height)
            {
                if (capture != null) { capture.Release(); Destroy(capture); }
                if (readback != null) Destroy(readback);
                capture = new RenderTexture(width, height, 0, RenderTextureFormat.ARGB32);
                capture.Create();
                readback = new Texture2D(width, height, TextureFormat.RGBA32, false);
            }
            var previous = RenderTexture.active;
            try
            {
                Graphics.Blit(camera, capture);
                RenderTexture.active = capture;
                readback.ReadPixels(new Rect(0, 0, width, height), 0, 0, false);
            }
            finally { RenderTexture.active = previous; }
            submittedAngle = camera.videoRotationAngle;
            submittedMirror = camera.videoVerticallyMirrored;
            bool quarter = submittedAngle == 90 || submittedAngle == 270;
            imageWidth = quarter ? height : width;
            imageHeight = quarter ? width : height;
            if (upright == null || upright.width != imageWidth || upright.height != imageHeight)
            {
                if (upright != null) Destroy(upright);
                upright = new Texture2D(imageWidth, imageHeight, TextureFormat.RGBA32, false);
                orientedPixels = new Color32[imageWidth * imageHeight];
            }
            var pixels = readback.GetRawTextureData<Color32>();
            for (int sy = 0; sy < height; sy++)
                for (int x = 0; x < width; x++)
                {
                    int y = submittedMirror ? height - 1 - sy : sy;
                    int ox = x, oy = y;
                    switch (submittedAngle)
                    {
                        case 90: ox = y; oy = width - 1 - x; break;
                        case 180: ox = width - 1 - x; oy = height - 1 - y; break;
                        case 270: ox = height - 1 - y; oy = x; break;
                    }
                    // MediaPipe consumes top-down image rows; Unity readback is bottom-up.
                    orientedPixels[(imageHeight - 1 - oy) * imageWidth + ox] = pixels[sy * width + x];
                }
            upright.SetPixels32(orientedPixels);
        }

        private void OnResult(HandLandmarkerResult result, Mediapipe.Image image, long time)
        {
            // Native worker callback: copy plain coordinates only. No Unity scene API calls.
            int count = result.handLandmarks == null ? 0 : result.handLandmarks.Count;
            var hands = new Vector2[count][];
            for (int h = 0; h < count; h++)
            {
                var points = result.handLandmarks[h].landmarks;
                hands[h] = new Vector2[points.Count];
                for (int i = 0; i < points.Count; i++) hands[h][i] = new Vector2(points[i].x, points[i].y);
            }
            lock (gate) { pending = hands; completed = true; }
        }

        private void OnGUI()
        {
            if (error != null)
            {
                GUI.Label(new Rect(24, 130, Screen.width - 48, 70), error, HearSayTheme.Label(20, Color.white));
                return;
            }
            if (graphic == null || !graphic.HasHands || string.IsNullOrWhiteSpace(Word)) return;
            // One shared translation centred between both wrists (single-signer demo).
            // If one hand is occluded, retain the label beside the remaining hand.
            Vector2 wrist = graphic.HandsCentreScreenPoint;
            float width = Mathf.Min(300f, Screen.safeArea.width - 32f);
            float x = Mathf.Clamp(wrist.x - width * 0.5f, Screen.safeArea.xMin + 16f, Screen.safeArea.xMax - width - 16f);
            float y = Mathf.Clamp(Screen.height - wrist.y - 85f, Screen.height - Screen.safeArea.yMax + 16f,
                Screen.height - Screen.safeArea.yMin - 96f);
            var rect = new Rect(x, y, width, 76);
            HearSayTheme.Card(rect, HearSayTheme.Panel);
            GUI.Label(new Rect(x + 14, y + 6, width - 28, 22), "ASL RECEIVED", HearSayTheme.Label(14, HearSayTheme.Accent));
            GUI.Label(new Rect(x + 14, y + 28, width - 28, 42), Word, HearSayTheme.Label(24, Color.white));
        }

        private void Fail(Exception e)
        {
            error = "Hand tracking unavailable — camera and ASL translation are still available.";
            Debug.LogError("Partner hand tracking: " + e);
            if (graphic != null) graphic.SetHands(null, 1, 1);
        }

        private void OnDestroy()
        {
            // Close joins native work before disposing the image's backing memory.
            try { detector?.Close(); }
            finally
            {
                detector = null;
                input?.Dispose();
                if (capture != null) { capture.Release(); Destroy(capture); }
                if (readback != null) Destroy(readback);
                if (upright != null) Destroy(upright);
                if (overlay != null) Destroy(overlay);
            }
        }
    }
}
