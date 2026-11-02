using System.Collections;
using UnityEngine;

namespace HearSay
{
    // VR-only. Shows the normal HearSay screen (camera feed, Canvas UI and OnGUI)
    // twice, side by side, for a phone placed in a Cardboard-style headset.
    //
    // It lives only in "HearSay VR Start" and survives into the scenes loaded from
    // there (HearSay Demo), so the normal HearSay scenes behave exactly as before.
    //
    // In the headset:
    //  - The screen stays still in each lens; turn your head to move the ring pointer.
    //  - Look at a button for a moment (or tap the screen / headset button) to press it.
    //  - Tapping while not looking at a button re-centres the view.
    //  - The split view is always on (there is no way to switch back to flat).
    [DefaultExecutionOrder(-1000)]
    public sealed class HearSayVrSplitScreen : MonoBehaviour
    {
        public const string VrStartScene = "HearSay VR Start";

        [Tooltip("Distance between the centres of the two pictures, in mm. Match your headset's lens spacing (most phone headsets: 60-64 mm).")]
        [Range(50f, 75f)] public float lensSeparationMm = 62f;
        [Tooltip("Width of each picture, in mm (it is made smaller if it would not fit).")]
        [Range(30f, 80f)] public float pictureWidthMm = 56f;
        [Tooltip("Minimum black gap between the two pictures in the middle of the screen, in mm.")]
        [Range(0f, 20f)] public float centreGapMm = 6f;
        [Tooltip("Degrees of head turn needed to pan across the whole screen.")]
        public float degreesAcrossScreen = 90f;
        [Tooltip("Pointer smoothing: higher follows the head faster, lower is steadier.")]
        [Range(2f, 30f)] public float pointerSmoothing = 10f;
        [Tooltip("Seconds of looking at a button before it is pressed.")]
        public float dwellSeconds = 2f;
        public bool invertHorizontal;
        public bool invertVertical;
        [Tooltip("Tick only if the picture appears upside down on your phone.")]
        public bool flipVertically;

        public static HearSayVrSplitScreen Instance { get; private set; }

        /// True while the two-lens view is showing.
        public static bool Active => Instance != null && Instance.split;

        private bool split = true;
        private RenderTexture frame;
        private Texture2D ring, ringOutline, fill;
        private Vector2 headOffset;          // gaze offset from screen centre, pixels, y up
        private int previousSleep;
        private string focusKey;
        private float focusStart, progress;
        private bool focusSeen, fired;
        private int tapFrame = -10;
        private float baseYaw;
        private float settleUntil;
        private bool hasBaseline, recenter;
        private Vector2 editorLook;

        private void Awake()
        {
            if (Instance != null && Instance != this) { Destroy(gameObject); return; }
            Instance = this;
            DontDestroyOnLoad(gameObject);
            previousSleep = Screen.sleepTimeout;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;
            Input.gyro.enabled = true;
            Input.compass.enabled = true; // used (if the phone has one) when there is no gyroscope
            settleUntil = Time.unscaledTime + 1f;
            ringOutline = MakeCircle(64, true, Color.white, 0.24f);
            ring = MakeCircle(64, true, HearSayTheme.Accent, 0.16f);
            fill = MakeCircle(64, false, HearSayTheme.Accent, 0f);
            StartCoroutine(Present());
        }

        private void OnDestroy()
        {
            if (Instance != this) return;
            Instance = null;
            Screen.sleepTimeout = previousSleep;
            if (frame != null) { frame.Release(); Destroy(frame); }
            if (ring != null) Destroy(ring);
            if (ringOutline != null) Destroy(ringOutline);
            if (fill != null) Destroy(fill);
        }

        private void Update()
        {
            // Drop focus when no button was under the pointer last frame.
            if (!focusSeen) { focusKey = null; progress = 0f; fired = false; }
            focusSeen = false;

            // VR only: the split view is always on, whatever is touched.

            TrackHead();

            if (SingleTap())
            {
                if (focusKey != null) tapFrame = Time.frameCount;
                else recenter = true; // re-centre on where you are looking now
            }
        }

        [Tooltip("No-gyroscope phones: degrees of pointer movement per degree of sideways head tilt.")]
        public float tiltToTurn = 1.2f;
        private bool gyroSeen;
        private Vector3 gravity;
        private float baseTilt;
        private float nextSensorLog;

        private void TrackHead()
        {
            float right = 0f, up = 0f; // degrees from where the user re-centred
            // Phones without a gyroscope (e.g. Galaxy A10), or whose orientation sensor
            // never starts, use the accelerometer: tilt up/down to move up/down and tilt
            // the head sideways (ear to shoulder) to move left/right.
            if (SystemInfo.supportsGyroscope && !gyroSeen)
            {
                Quaternion a0 = Input.gyro.attitude;
                if (Mathf.Abs(a0.w) < 0.9999f || a0.x != 0f || a0.y != 0f || a0.z != 0f) gyroSeen = true;
            }
            bool useGyro = SystemInfo.supportsGyroscope && (gyroSeen || Time.unscaledTime < 3f);
            if (Time.unscaledTime >= nextSensorLog)
            {
                nextSensorLog = Time.unscaledTime + 5f;
                Debug.Log($"HearSay VR sensors: gyroSupported={SystemInfo.supportsGyroscope} gyroEnabled={Input.gyro.enabled} " +
                          $"attitude={Input.gyro.attitude} rate={Input.gyro.rotationRateUnbiased} accel={Input.acceleration} " +
                          $"useGyro={useGyro} orientation={Screen.orientation} offset={headOffset} " +
                          $"compass={Input.compass.enabled} raw={Input.compass.rawVector} heading={Input.compass.magneticHeading} ts={Input.compass.timestamp}");
            }
            if (!useGyro && SystemInfo.supportsAccelerometer)
            {
                // Smoothed gravity. Unity reports it in screen axes: x = screen right,
                // y = screen up, z = out of the screen (towards the eyes).
                gravity = Vector3.Lerp(gravity, Input.acceleration, 1f - Mathf.Exp(-3f * Time.unscaledDeltaTime));
                if (gravity.sqrMagnitude > 0.01f)
                {
                    Vector3 g = gravity.normalized;
                    float pitch = Mathf.Asin(Mathf.Clamp(g.z, -1f, 1f)) * Mathf.Rad2Deg;
                    // Sideways head tilt, 0 when the phone is held level; + = tilted right.
                    float tilt = Mathf.Atan2(g.x, -g.y) * Mathf.Rad2Deg;
                    // Level = centre (like up/down). A tap while not looking at a button
                    // re-centres on the current tilt.
                    if (recenter) { baseTilt = tilt; recenter = false; }
                    // Small dead zone so head movement and shake don't nudge it sideways.
                    float t = Mathf.DeltaAngle(baseTilt, tilt);
                    t = Mathf.Sign(t) * Mathf.Max(0f, Mathf.Abs(t) - 5f);
                    right = Mathf.Clamp(t * tiltToTurn,
                        -degreesAcrossScreen * 0.5f, degreesAcrossScreen * 0.5f);
                    // Lying flat (e.g. on a desk) has no meaningful sideways tilt: stay centred.
                    float upright = new Vector2(g.x, g.y).magnitude;
                    right *= Mathf.Clamp01((upright - 0.2f) / 0.3f);
                    up = pitch;
                }
            }
            else if (useGyro)
            {
                // Absolute phone orientation (sensor fusion), so turning follows the
                // head 1:1 without drift from adding up rotation speeds.
                Quaternion q = Input.gyro.attitude;
                // The sensor reports a blank (identity) orientation for a moment after
                // start-up; never take "straight ahead" from that.
                bool sensorReady = Mathf.Abs(q.w) < 0.9999f || q.x != 0f || q.y != 0f || q.z != 0f;
                if (!sensorReady) settleUntil = Time.unscaledTime + 0.75f;
                Quaternion phone = Quaternion.Euler(90f, 0f, 0f) * new Quaternion(q.x, q.y, -q.z, -q.w);
                Vector3 f = phone * Vector3.forward; // where the rear camera points
                float yaw = Mathf.Atan2(f.x, f.z) * Mathf.Rad2Deg;
                float pitch = Mathf.Asin(Mathf.Clamp(f.y, -1f, 1f)) * Mathf.Rad2Deg;
                // Up/down is measured from level: phone upright and facing straight
                // ahead puts the pointer in the centre. Left/right is measured from
                // the direction faced at start (or when re-centred with a tap).
                // Keep re-centring until the sensor has settled for a moment after start-up.
                if (!hasBaseline || recenter || Time.unscaledTime < settleUntil)
                { baseYaw = yaw; hasBaseline = true; recenter = false; }
                right = Mathf.DeltaAngle(baseYaw, yaw);
                // Turning past the edge drags "straight ahead" along, so the pointer stays
                // at the edge and comes straight back, instead of flipping to the other side.
                float halfRange = degreesAcrossScreen * 0.5f;
                if (right > halfRange) { baseYaw = Mathf.Repeat(yaw - halfRange + 180f, 360f) - 180f; right = halfRange; }
                else if (right < -halfRange) { baseYaw = Mathf.Repeat(yaw + halfRange + 180f, 360f) - 180f; right = -halfRange; }
                up = pitch;
            }
#if UNITY_EDITOR
            // Editor preview: arrow keys stand in for head movement.
            editorLook.x += ((Input.GetKey(KeyCode.RightArrow) ? 1f : 0f) - (Input.GetKey(KeyCode.LeftArrow) ? 1f : 0f)) * 60f * Time.unscaledDeltaTime;
            editorLook.y += ((Input.GetKey(KeyCode.UpArrow) ? 1f : 0f) - (Input.GetKey(KeyCode.DownArrow) ? 1f : 0f)) * 60f * Time.unscaledDeltaTime;
            if (recenter) { editorLook = Vector2.zero; recenter = false; }
            right += editorLook.x;
            up += editorLook.y;
#endif
            if (invertHorizontal) right = -right;
            if (invertVertical) up = -up;
            float pixelsPerDegree = Screen.width / degreesAcrossScreen;
            Vector2 target = new Vector2(right, up) * pixelsPerDegree;
            headOffset = Vector2.Lerp(headOffset, target, 1f - Mathf.Exp(-pointerSmoothing * Time.unscaledDeltaTime));
            headOffset.x = Mathf.Clamp(headOffset.x, -Screen.width * 0.5f, Screen.width * 0.5f);
            headOffset.y = Mathf.Clamp(headOffset.y, -Screen.height * 0.5f, Screen.height * 0.5f);
        }

        private static bool SingleTap()
        {
            if (Input.touchCount > 1) return false;
            return Input.GetMouseButtonDown(0);
        }

        // Pointer position in OnGUI screen coordinates (origin top-left).
        private Vector2 GazeGuiPoint()
        {
            return new Vector2(Screen.width * 0.5f + headOffset.x, Screen.height * 0.5f - headOffset.y);
        }

        /// Called from HearSayTheme.Action inside OnGUI. Returns true once when the
        /// button is pressed by looking at it (dwell) or by tapping while looking at it.
        public static bool GazeButton(Rect guiRect, string title, out bool focused, out float dwell)
        {
            focused = false;
            dwell = 0f;
            if (!Active) return false;
            var self = Instance;
            Vector2 local = GUI.matrix.inverse.MultiplyPoint3x4(self.GazeGuiPoint());
            if (!guiRect.Contains(local)) return false;

            focused = true;
            self.focusSeen = true;
            string key = title + guiRect;
            if (key != self.focusKey)
            {
                self.focusKey = key;
                self.focusStart = Time.unscaledTime;
                self.fired = false;
            }
            self.progress = Mathf.Clamp01((Time.unscaledTime - self.focusStart) / self.dwellSeconds);
            dwell = self.progress;
            if (Event.current == null || Event.current.type != EventType.Repaint) return false;
            bool tapped = Time.frameCount - self.tapFrame <= 1;
            if (self.progress >= 1f || tapped)
            {
                // Keep looking to press again (e.g. the first press only connects
                // to the server; the second one opens the scene), like tapping twice.
                self.focusStart = Time.unscaledTime + 1f;
                self.progress = 0f;
                self.tapFrame = -10;
                return true;
            }
            return false;
        }

        private IEnumerator Present()
        {
            var endOfFrame = new WaitForEndOfFrame();
            while (true)
            {
                yield return endOfFrame;
                if (!split) continue;
                int w = Screen.width, h = Screen.height;
                if (frame == null || frame.width != w || frame.height != h)
                {
                    if (frame != null) { frame.Release(); Destroy(frame); }
                    frame = new RenderTexture(w, h, 0, RenderTextureFormat.ARGB32) { name = "HearSay VR frame" };
                    frame.Create();
                }
                // Everything the normal app drew this frame (camera, Canvas, OnGUI).
                ScreenCapture.CaptureScreenshotIntoRenderTexture(frame);
                DrawEyes(w, h);
            }
        }

        private void DrawEyes(int w, int h)
        {
            RenderTexture.active = null;
            bool flip = SystemInfo.graphicsUVStartsAtTop != flipVertically;
            float halfW = w * 0.5f;
            // Place the two pictures by real size on the screen so their centres sit
            // under the headset's lenses, with a black gap between them.
            float dpi = Screen.dpi > 50f ? Screen.dpi : 400f; // some phones report 0
            float pxPerMm = dpi / 25.4f;
            float halfSep = Mathf.Min(lensSeparationMm * 0.5f * pxPerMm, halfW * 0.9f);
            float eyeW = pictureWidthMm * pxPerMm;
            eyeW = Mathf.Min(eyeW, 2f * (halfSep - centreGapMm * 0.5f * pxPerMm)); // keep the middle gap
            eyeW = Mathf.Min(eyeW, 2f * (halfW - halfSep));                         // stay on screen
            float eyeH = eyeW * h / w;
            if (eyeH > h) { eyeH = h; eyeW = eyeH * w / h; }
            float s = eyeW / w;

            GL.PushMatrix();
            GL.Viewport(new Rect(0, 0, w, h));
            GL.Clear(true, true, Color.black);
            for (int eye = 0; eye < 2; eye++)
            {
                // Each eye is clipped to its own half of the screen.
                GL.Viewport(new Rect(eye * halfW, 0, halfW, h));
                GL.LoadPixelMatrix(0, halfW, h, 0); // top-left origin, like OnGUI
                // Lens centre in this half's coordinates (left half: from its left edge,
                // right half: from the middle of the screen).
                float cx = eye == 0 ? halfW - halfSep : halfSep;
                float cy = h * 0.5f;
                // The picture stays fixed in each lens; only the pointer moves with the head.
                var r = new Rect(cx - eyeW * 0.5f, cy - eyeH * 0.5f, eyeW, eyeH);
                if (flip) r = new Rect(r.x, r.yMax, r.width, -r.height);
                Graphics.DrawTexture(r, frame);

                // Pointer: where the head is aimed, drawn at the same spot in both eyes.
                float px = cx + headOffset.x * s, py = cy - headOffset.y * s;
                float ringSize = Mathf.Max(28f, eyeH * 0.09f);
                var ringRect = new Rect(px - ringSize * 0.5f, py - ringSize * 0.5f, ringSize, ringSize);
                Graphics.DrawTexture(ringRect, ringOutline); // white edge: visible on any background
                Graphics.DrawTexture(ringRect, ring);
                if (progress > 0f)
                {
                    float f = ringSize * progress;
                    Graphics.DrawTexture(new Rect(px - f * 0.5f, py - f * 0.5f, f, f), fill);
                }
            }
            GL.Viewport(new Rect(0, 0, w, h));
            GL.PopMatrix();
        }

        // thickness: ring width as a fraction of the size (outline only).
        private static Texture2D MakeCircle(int size, bool outline, Color color, float thickness)
        {
            var tex = new Texture2D(size, size, TextureFormat.RGBA32, false) { hideFlags = HideFlags.HideAndDontSave };
            var pixels = new Color[size * size];
            float c = (size - 1) * 0.5f, outer = size * 0.5f - 1f, inner = outer - size * thickness;
            Color accent = color;
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                {
                    float d = Vector2.Distance(new Vector2(x, y), new Vector2(c, c));
                    float a = Mathf.Clamp01(outer - d + 0.5f);
                    if (outline) a *= Mathf.Clamp01(d - inner + 0.5f);
                    pixels[y * size + x] = new Color(accent.r, accent.g, accent.b, a);
                }
            tex.SetPixels(pixels);
            tex.Apply();
            return tex;
        }
    }
}
