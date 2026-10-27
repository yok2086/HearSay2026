using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

namespace HearSay
{
    public sealed class HearSayVrHomeCanvas : MonoBehaviour
    {
        private const float Width = 1000f;
        private const float Height = 650f;
        private Canvas canvas;
        private RectTransform deafButton;
        private RectTransform partnerButton;
        private float gazeSince = -1f;
        private int gazeTarget = -1;

        private void Start()
        {
            if (SceneManager.GetActiveScene().name != "Final VR HearSay") return;
            Build();
        }

        private void Build()
        {
            var camera = Camera.main;
            if (camera == null) return;

            var root = new GameObject("HearSay VR Home Canvas");
            root.transform.SetParent(camera.transform, false);
            // Shinecon/Cardboard lenses need a large, close virtual panel so the
            // role choices fill each eye instead of appearing as a tiny card.
            root.transform.localPosition = new Vector3(0f, 0f, 1.15f);
            root.transform.localRotation = Quaternion.identity;
            root.transform.localScale = Vector3.one * 0.0022f;
            canvas = root.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.WorldSpace;
            canvas.worldCamera = camera;
            canvas.pixelPerfect = false;
            var rect = canvas.GetComponent<RectTransform>();
            rect.sizeDelta = new Vector2(Width, Height);
            root.AddComponent<GraphicRaycaster>();

            var reticle = AddText(root.transform, "○", 475, 295, 50, 50, 38,
                new Color(.1f, .85f, .9f), TextAnchor.MiddleCenter);
            reticle.name = "VR Gaze Reticle";

            AddText(root.transform, "HEARSAY  /  CONNECTED CONVERSATIONS", 60, 28, 880, 32, 16, new Color(.1f, .85f, .9f));
            AddText(root.transform, "A clearer conversation.", 60, 70, 880, 65, 46, Color.white);
            AddText(root.transform, "Choose how you want to connect.", 60, 140, 880, 40, 23, new Color(.65f, .72f, .82f));

            deafButton = AddButton(root.transform, "Deaf user\n\nFind the speaker and follow live captions.\n\nOPEN LIVE CAPTIONS", 60, 205, 425, 220, 30, () => OpenRole(1));
            partnerButton = AddButton(root.transform, "Conversation partner\n\nSee sign-language translations in your surroundings.\n\nOPEN ASL TRANSLATION", 505, 205, 435, 220, 28, () => OpenRole(0));
            AddText(root.transform, "Data monitor / teammate", 60, 448, 310, 48, 18, Color.white, TextAnchor.MiddleCenter);
            AddText(root.transform, "Connection settings   +", 390, 448, 550, 48, 19, Color.white, TextAnchor.MiddleCenter);
        }

        private static Text AddText(Transform parent, string value, float x, float y, float w, float h, int size, Color color, TextAnchor anchor = TextAnchor.UpperLeft)
        {
            var go = new GameObject("Text"); go.transform.SetParent(parent, false);
            var text = go.AddComponent<Text>(); text.text = value; text.font = Resources.GetBuiltinResource<Font>("Arial.ttf");
            text.fontSize = size; text.color = color; text.alignment = anchor; text.horizontalOverflow = HorizontalWrapMode.Wrap; text.verticalOverflow = VerticalWrapMode.Overflow;
            var r = text.rectTransform; r.anchorMin = r.anchorMax = new Vector2(0f, 1f); r.pivot = new Vector2(0f, 1f); r.anchoredPosition = new Vector2(x, -y); r.sizeDelta = new Vector2(w, h); return text;
        }

        private static RectTransform AddButton(Transform parent, string value, float x, float y, float w, float h, int size, UnityEngine.Events.UnityAction action)
        {
            var go = new GameObject("VR Role Button"); go.transform.SetParent(parent, false); var image = go.AddComponent<Image>(); image.color = new Color(.055f, .08f, .13f, .98f);
            var button = go.AddComponent<Button>(); button.targetGraphic = image; button.onClick.AddListener(action);
            var text = AddText(go.transform, value, 28, 24, w - 56, h - 48, size, Color.white); text.alignment = TextAnchor.UpperLeft;
            var r = go.GetComponent<RectTransform>(); r.anchorMin = r.anchorMax = new Vector2(0f, 1f); r.pivot = new Vector2(0f, 1f); r.anchoredPosition = new Vector2(x, -y); r.sizeDelta = new Vector2(w, h); return r;
        }

        private void Update()
        {
            if (canvas == null || Camera.main == null) return;
            var screen = new Vector2(Screen.width * .5f, Screen.height * .5f);
            int next = -1;
            if (RectTransformUtility.RectangleContainsScreenPoint(deafButton, screen, Camera.main)) next = 0;
            else if (RectTransformUtility.RectangleContainsScreenPoint(partnerButton, screen, Camera.main)) next = 1;
            if (next != gazeTarget) { gazeTarget = next; gazeSince = Time.unscaledTime; }
            if (gazeTarget >= 0 && Time.unscaledTime - gazeSince >= 3f) { gazeSince = float.PositiveInfinity; (gazeTarget == 0 ? deafButton : partnerButton).GetComponent<Button>().onClick.Invoke(); }
        }

        private static void OpenRole(int role)
        {
            PlayerPrefs.SetInt("HearSay.VrRole", role); PlayerPrefs.Save(); SceneManager.LoadScene("HearSay Demo");
        }
    }
}
