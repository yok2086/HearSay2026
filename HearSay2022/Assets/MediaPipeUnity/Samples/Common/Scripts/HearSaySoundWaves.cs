using UnityEngine;
using UnityEngine.UI;

namespace HearSay
{
    // Lightweight UI mesh: no textures, particles, camera changes, or audio recording.
    [RequireComponent(typeof(CanvasRenderer))]
    public class HearSaySoundWaves : MaskableGraphic
    {
        [SerializeField, Range(0.5f, 4f)] private float cycleSeconds = 1.8f;
        [SerializeField, Range(60f, 400f)] private float waveReach = 220f;
        [SerializeField, Range(2f, 12f)] private float lineWidth = 5f;
        private bool right;
        private float startedAt;

        public void Show(bool visible, bool onRight)
        {
            if (visible && (!gameObject.activeSelf || right != onRight)) startedAt = Time.unscaledTime;
            right = onRight;
            if (gameObject.activeSelf != visible) gameObject.SetActive(visible);
        }

        protected override void Awake()
        {
            base.Awake();
            raycastTarget = false;
        }

        private void Update() { SetVerticesDirty(); }

        protected override void OnPopulateMesh(VertexHelper vh)
        {
            vh.Clear();
            Rect r = rectTransform.rect;
            float reach = Mathf.Min(waveReach, r.width * 0.2f, r.height * 0.4f);
            Vector2 origin = new Vector2(right ? r.xMax - 16f : r.xMin + 16f, r.center.y);
            for (int ring = 0; ring < 3; ring++)
            {
                float phase = Mathf.Repeat((Time.unscaledTime - startedAt) / Mathf.Max(0.5f, cycleSeconds) + ring / 3f, 1f);
                float radius = Mathf.Lerp(22f, reach, phase);
                float alpha = Mathf.Sin(phase * Mathf.PI) * color.a;
                DrawArc(vh, origin, radius, lineWidth * 3f, new Color(color.r, color.g, color.b, alpha * 0.12f));
                DrawArc(vh, origin, radius, lineWidth, new Color(color.r, color.g, color.b, alpha));
            }
        }

        private void DrawArc(VertexHelper vh, Vector2 origin, float radius, float width, Color tint)
        {
            const int segments = 32;
            int start = vh.currentVertCount;
            for (int i = 0; i <= segments; i++)
            {
                float angle = Mathf.Lerp(-65f, 65f, i / (float)segments) * Mathf.Deg2Rad;
                Vector2 direction = new Vector2(Mathf.Cos(angle) * (right ? -1f : 1f), Mathf.Sin(angle));
                vh.AddVert(origin + direction * (radius - width * 0.5f), tint, Vector2.zero);
                vh.AddVert(origin + direction * (radius + width * 0.5f), tint, Vector2.zero);
                if (i == segments) continue;
                int k = start + i * 2;
                vh.AddTriangle(k, k + 1, k + 2);
                vh.AddTriangle(k + 1, k + 3, k + 2);
            }
        }
    }
}
