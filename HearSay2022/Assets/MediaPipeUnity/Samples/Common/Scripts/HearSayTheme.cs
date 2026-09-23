using UnityEngine;

namespace HearSay
{
    // Shared presentation only. No device, network, or tracking state.
    public static class HearSayTheme
    {
        public static readonly Color Navy = new Color(0.035f, 0.055f, 0.09f);
        public static readonly Color Panel = new Color(0.055f, 0.085f, 0.13f, 0.94f);
        public static readonly Color Accent = new Color(0.25f, 0.85f, 0.9f);
        public static readonly Color Muted = new Color(0.68f, 0.75f, 0.83f);
        private static Texture2D rounded;
        private static Sprite sprite;

        public static Texture2D Rounded
        {
            get
            {
                if (rounded != null) return rounded;
                const int size = 64;
                rounded = new Texture2D(size, size, TextureFormat.RGBA32, false);
                rounded.hideFlags = HideFlags.HideAndDontSave;
                rounded.wrapMode = TextureWrapMode.Clamp;
                rounded.filterMode = FilterMode.Bilinear;
                var pixels = new Color[size * size];
                for (int y = 0; y < size; y++)
                    for (int x = 0; x < size; x++)
                    {
                        float dx = Mathf.Max(Mathf.Abs(x - 31.5f) - 17.5f, 0);
                        float dy = Mathf.Max(Mathf.Abs(y - 31.5f) - 17.5f, 0);
                        pixels[y * size + x] = new Color(1, 1, 1, Mathf.Clamp01(14f - Mathf.Sqrt(dx * dx + dy * dy)));
                    }
                rounded.SetPixels(pixels);
                rounded.Apply();
                return rounded;
            }
        }

        public static Sprite RoundedSprite
        {
            get
            {
                if (sprite == null)
                    sprite = Sprite.Create(Rounded, new Rect(0, 0, 64, 64), new Vector2(0.5f, 0.5f), 100, 0,
                        SpriteMeshType.FullRect, new Vector4(18, 18, 18, 18));
                return sprite;
            }
        }

        public static GUIStyle Label(int size, Color color, TextAnchor align = TextAnchor.MiddleLeft)
        {
            var style = new GUIStyle(GUI.skin.label) { fontSize = size, alignment = align, wordWrap = true };
            style.normal.textColor = color;
            return style;
        }

        public static GUIStyle Button(int size)
        {
            var style = new GUIStyle(GUI.skin.button)
            {
                fontSize = size, border = new RectOffset(18, 18, 18, 18),
                padding = new RectOffset(22, 22, 12, 12), alignment = TextAnchor.MiddleCenter
            };
            style.normal.background = Rounded;
            style.hover.background = Rounded;
            style.active.background = Rounded;
            style.focused.background = Rounded;
            style.normal.textColor = Color.white;
            style.hover.textColor = Accent;
            style.active.textColor = Accent;
            style.focused.textColor = Accent;
            return style;
        }

        public static void Card(Rect rect, Color tint)
        {
            var old = GUI.color;
            GUI.color = tint;
            GUI.Box(rect, GUIContent.none, new GUIStyle { normal = { background = Rounded }, border = new RectOffset(18, 18, 18, 18) });
            GUI.color = old;
        }

        public static bool Action(Rect rect, string title, int size = 22)
        {
            var old = GUI.backgroundColor;
            GUI.backgroundColor = Panel;
            bool pressed = GUI.Button(rect, title, Button(size));
            GUI.backgroundColor = old;
            return pressed;
        }
    }
}
