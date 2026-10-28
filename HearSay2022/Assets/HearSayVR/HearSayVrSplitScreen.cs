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

        [Tooltip("Size of each eye image relative to half the screen width (1 = touch the edges).")]
        [Range(0.5f, 1.5f)] public float zoom = 1f;
        [Tooltip("Pixels to move both eye images apart (+) or together (-) to line up with the lenses.")]
        public float lensOffsetPixels = 0f;
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
        private Texture2D ring, fill;
        private Vector2 headOffset;          // gaze offset from screen centre, pixels, y up
        private int previousSleep;
        private string focusKey;
        private float focusStart, progress;
        private bool focusSeen, fired;
        private int tapFrame = -10;
        private float baseYaw;
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
            ring = MakeCircle(48, true);
            fill = MakeCircle(48, false);
            StartCoroutine(Present());
        }

        private void OnDestroy()
        {
            if (Instance != this) return;
            Instance = null;
            Screen.sleepTimeout = previousSleep;
            if (frame != null) { frame.Release(); Destroy(frame); }
            if (ring != null) Destroy(ring);
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

        private void TrackHead()
        {
            float right = 0f, up = 0f; // degrees from where the user re-centred
            if (SystemInfo.supportsGyroscope)
            {
                // Absolute phone orientation (sensor fusion), so turning follows the
                // head 1:1 without drift from adding up rotation speeds.
                Quaternion q = Input.gyro.attitude;
                Quaternion phone = Quaternion.Euler(90f, 0f, 0f) * new Quaternion(q.x, q.y, -q.z, -q.w);
                Vector3 f = phone * Vector3.forward; // where the rear camera points
                float yaw = Mathf.Atan2(f.x, f.z) * Mathf.Rad2Deg;
                float pitch = Mathf.Asin(Mathf.Clamp(f.y, -1f, 1f)) * Mathf.Rad2Deg;
                // Up/down is measured from level: phone upright and facing straight
                // ahead puts the pointer in the centre. Left/right is measured from
                // the direction faced at start (or when re-centred with a tap).
                if (!hasBaseline || recenter) { baseYaw = yaw; hasBaseline = true; recenter = false; }
                right = Mathf.DeltaAngle(baseYaw, yaw);
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
            float eyeW = halfW * zoom, eyeH = eyeW * h / w, s = eyeW / w;

            GL.PushMatrix();
            GL.Viewport(new Rect(0, 0, w, h));
            GL.Clear(true, true, Color.black);
            for (int eye = 0; eye < 2; eye++)
            {
                // Each eye is clipped to its own half of the screen.
                GL.Viewport(new Rect(eye * halfW, 0, halfW, h));
                GL.LoadPixelMatrix(0, halfW, h, 0); // top-left origin, like OnGUI
                float cx = halfW * 0.5f + (eye == 0 ? -lensOffsetPixels : lensOffsetPixels);
                float cy = h * 0.5f;
                // The picture stays fixed in each lens; only the pointer moves with the head.
                var r = new Rect(cx - eyeW * 0.5f, cy - eyeH * 0.5f, eyeW, eyeH);
                if (flip) r = new Rect(r.x, r.yMax, r.width, -r.height);
                Graphics.DrawTexture(r, frame);

                // Pointer: where the head is aimed, drawn at the same spot in both eyes.
                float px = cx + headOffset.x * s, py = cy - headOffset.y * s;
                float ringSize = Mathf.Max(18f, h * 0.035f);
                Graphics.DrawTexture(new Rect(px - ringSize * 0.5f, py - ringSize * 0.5f, ringSize, ringSize), ring);
                if (progress > 0f)
                {
                    float f = ringSize * progress;
                    Graphics.DrawTexture(new Rect(px - f * 0.5f, py - f * 0.5f, f, f), fill);
                }
            }
            GL.Viewport(new Rect(0, 0, w, h));
            GL.PopMatrix();
        }

        private static Texture2D MakeCircle(int size, bool outline)
        {
            var tex = new Texture2D(size, size, TextureFormat.RGBA32, false) { hideFlags = HideFlags.HideAndDontSave };
            var pixels = new Color[size * size];
            float c = (size - 1) * 0.5f, outer = size * 0.5f - 1f, inner = outer - size * 0.14f;
            Color accent = HearSayTheme.Accent;
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
