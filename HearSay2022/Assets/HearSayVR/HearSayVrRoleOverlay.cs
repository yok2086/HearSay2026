using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace HearSay
{
    // VR-only replacement for the role-scene screen-space labels. The original
    // detector, microphone, Whisper stream, and MQTT/HTTP reporting remain intact.
    public sealed class HearSayVrRoleOverlay : MonoBehaviour
    {
        private Canvas canvas;
        private Text speakerText;
        private Text captionText;
        private Text microphoneText;
        private RectTransform homeButton;
        private Transform cursor;
        private Quaternion calibration;
        private float gazeStarted = -1f;
        private bool cursorReady;

        private void Start()
        {
            if (SceneManager.GetActiveScene().name != "HearSay Demo") return;
            var camera = Camera.main;
            if (camera == null) return;
            var root = new GameObject("HearSay VR Role Overlay");
            root.transform.position = camera.transform.position + camera.transform.forward * 1.25f;
            root.transform.rotation = camera.transform.rotation;
            root.transform.localScale = Vector3.one * 0.0022f;
            canvas = root.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.WorldSpace;
            canvas.worldCamera = camera;
            canvas.GetComponent<RectTransform>().sizeDelta = new Vector2(1100f, 650f);
            root.AddComponent<GraphicRaycaster>();

            // Keep the header items in separate, non-overlapping regions:
            // speaker state on the left, mic state in the middle, Home on the right.
            speakerText = AddText(root.transform, "No speaker detected", 40, 35, 360, 50, 25, Color.white, TextAnchor.MiddleLeft);
            microphoneText = AddText(root.transform, "●", 690, 35, 55, 45, 32, Color.red, TextAnchor.MiddleCenter);
            captionText = AddText(root.transform, "Waiting for speaker…", 260, 270, 580, 120, 30, Color.white, TextAnchor.MiddleCenter);
            homeButton = AddButton(root.transform, "Home", 875, 35, 175, 60);
            CreateCursor(camera);
#if UNITY_ANDROID && !UNITY_EDITOR
            Input.gyro.enabled = true;
            calibration = Quaternion.Inverse(Convert(Input.gyro.attitude));
            cursorReady = true;
#endif
        }

        private void Update()
        {
            if (canvas == null || Camera.main == null || cursor == null) return;
            if (!HearSayRoles.DeafRole)
            {
                Debug.Log("HearSay VR role overlay: ASL partner mode");
                speakerText.text = "ASL translation";
                speakerText.color = new Color(.1f, .85f, .9f);
                microphoneText.text = "";
                captionText.text = HearSayRoles.CurrentTranslation.Replace("ASL DETECTED\n", "");
                return;
            }
            // This is the same locked-speaker state that makes the yellow
            // outline appear, so the header cannot disagree with the outline.
            bool speakerDetected = SpeakerActivity.IsSpeaking;
            speakerText.text = speakerDetected ? "Speaker detected" : "No speaker detected";
            speakerText.color = speakerDetected ? new Color(1f, .87f, .3f) : Color.white;
            microphoneText.color = HearSayLiveCaption.MicrophoneRecording ? new Color(.15f, 1f, .3f) : Color.red;
            captionText.text = string.IsNullOrEmpty(HearSayLiveCaption.LatestCaption)
                ? (SpeakerActivity.IsSpeaking ? "Listening…" : "")
                : HearSayLiveCaption.LatestCaption;
            if (SpeakerActivity.HasMouthPosition)
            {
                float x = Mathf.Clamp01(SpeakerActivity.CurrentMouthPosition.x);
                float y = 1f - Mathf.Clamp01(SpeakerActivity.CurrentMouthPosition.y);
                captionText.rectTransform.anchoredPosition = new Vector2(x * 1100f - 290f, -y * 650f - 80f);
            }

#if UNITY_ANDROID && !UNITY_EDITOR
            if (cursorReady)
            {
                Vector3 angles = (calibration * Convert(Input.gyro.attitude)).eulerAngles;
                float yaw = Normalize(angles.y);
                float pitch = Normalize(angles.x);
                float x = Mathf.Clamp(yaw / 35f, -1f, 1f) * .60f;
                float y = Mathf.Clamp(-pitch / 25f, -1f, 1f) * .42f;
                cursor.position = Camera.main.transform.position + Camera.main.transform.rotation * new Vector3(x, y, 1.05f);
                cursor.rotation = Camera.main.transform.rotation;
            }
#endif
            var point = Camera.main.WorldToScreenPoint(cursor.position);
            bool overHome = RectTransformUtility.RectangleContainsScreenPoint(homeButton, point, Camera.main);
            if (overHome)
            {
                if (gazeStarted < 0f) gazeStarted = Time.unscaledTime;
                if (Time.unscaledTime - gazeStarted >= 5f)
                {
                    gazeStarted = float.PositiveInfinity;
                    HearSayRoles.ReturnToSelector();
                }
            }
            else gazeStarted = -1f;
        }

        private void CreateCursor(Camera camera)
        {
            var go = new GameObject("VR Role Gaze Cursor");
            cursor = go.transform;
            cursor.position = camera.transform.position + camera.transform.forward * 1.05f;
            cursor.rotation = camera.transform.rotation;
            cursor.localScale = Vector3.one * .0022f;
            var c = go.AddComponent<Canvas>(); c.renderMode = RenderMode.WorldSpace; c.worldCamera = camera;
            c.GetComponent<RectTransform>().sizeDelta = new Vector2(100f, 100f);
            AddText(go.transform, "○", 0, 0, 100, 100, 38, new Color(.1f, .85f, .9f), TextAnchor.MiddleCenter);
        }

        private static Text AddText(Transform parent, string value, float x, float y, float w, float h, int size, Color color, TextAnchor anchor = TextAnchor.UpperLeft)
        {
            var go = new GameObject("VR Label"); go.transform.SetParent(parent, false);
            var text = go.AddComponent<Text>(); text.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf"); text.text = value;
            text.fontSize = size; text.color = color; text.alignment = anchor; text.horizontalOverflow = HorizontalWrapMode.Wrap; text.verticalOverflow = VerticalWrapMode.Overflow;
            var r = text.rectTransform; r.anchorMin = r.anchorMax = new Vector2(0f, 1f); r.pivot = new Vector2(0f, 1f); r.anchoredPosition = new Vector2(x, -y); r.sizeDelta = new Vector2(w, h); return text;
        }

        private static RectTransform AddButton(Transform parent, string label, float x, float y, float w, float h)
        {
            var go = new GameObject("VR Home Button"); go.transform.SetParent(parent, false); var image = go.AddComponent<Image>(); image.color = new Color(.055f, .08f, .13f, .98f);
            var r = go.GetComponent<RectTransform>(); r.anchorMin = r.anchorMax = new Vector2(0f, 1f); r.pivot = new Vector2(0f, 1f); r.anchoredPosition = new Vector2(x, -y); r.sizeDelta = new Vector2(w, h);
            AddText(go.transform, label, 0, 0, w, h, 24, Color.white, TextAnchor.MiddleCenter); return r;
        }

        private static float Normalize(float angle) => angle > 180f ? angle - 360f : angle;
        private static Quaternion Convert(Quaternion q) => new Quaternion(q.x, q.y, -q.z, -q.w);
    }
}
