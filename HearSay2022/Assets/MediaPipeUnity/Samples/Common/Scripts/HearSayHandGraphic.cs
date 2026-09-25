using UnityEngine;
using UnityEngine.UI;

namespace HearSay
{
    public class HearSayHandGraphic : MaskableGraphic
    {
        private static readonly int[] Edges = {
            0,1, 1,2, 2,3, 3,4, 0,5, 5,6, 6,7, 7,8,
            5,9, 9,10, 10,11, 11,12, 9,13, 13,14, 14,15, 15,16,
            13,17, 0,17, 17,18, 18,19, 19,20 };
        private Vector2[][] hands;
        private float width, height;
        public bool HasHands => hands != null && hands.Length > 0 && hands[0].Length >= 21;
        public Vector2 HandsCentreScreenPoint
        {
            get
            {
                if (!HasHands) return Vector2.zero;
                Vector2 centre = Vector2.zero;
                int count = 0;
                foreach (var hand in hands)
                {
                    if (hand == null || hand.Length < 21) continue;
                    centre += Map(hand[0]);
                    count++;
                }
                return centre / Mathf.Max(1, count) + new Vector2(Screen.width, Screen.height) * 0.5f;
            }
        }

        public void SetHands(Vector2[][] points, int imageWidth, int imageHeight)
        {
            hands = points;
            float scale = Mathf.Max(rectTransform.rect.width / Mathf.Max(1, imageWidth),
                rectTransform.rect.height / Mathf.Max(1, imageHeight));
            width = imageWidth * scale;
            height = imageHeight * scale;
            SetVerticesDirty();
        }

        private Vector2 Map(Vector2 point) => new Vector2((point.x - 0.5f) * width, (0.5f - point.y) * height);

        protected override void OnPopulateMesh(VertexHelper vh)
        {
            vh.Clear();
            if (hands == null) return;
            foreach (var hand in hands)
            {
                if (hand.Length < 21) continue;
                for (int i = 0; i < Edges.Length; i += 2)
                {
                    Vector2 a = Map(hand[Edges[i]]), b = Map(hand[Edges[i + 1]]);
                    Vector2 delta = b - a;
                    Vector2 normal = new Vector2(-delta.y, delta.x).normalized * 1.7f;
                    int n = vh.currentVertCount;
                    vh.AddVert(a - normal, color, Vector2.zero); vh.AddVert(a + normal, color, Vector2.zero);
                    vh.AddVert(b + normal, color, Vector2.zero); vh.AddVert(b - normal, color, Vector2.zero);
                    vh.AddTriangle(n, n + 1, n + 2); vh.AddTriangle(n, n + 2, n + 3);
                }
                foreach (var p in hand)
                {
                    Vector2 centre = Map(p);
                    int n = vh.currentVertCount;
                    vh.AddVert(centre, Color.white, Vector2.zero);
                    for (int j = 0; j <= 8; j++)
                    {
                        float angle = j * Mathf.PI / 4;
                        vh.AddVert(centre + new Vector2(Mathf.Cos(angle), Mathf.Sin(angle)) * 3.6f, color, Vector2.zero);
                        if (j > 0) vh.AddTriangle(n, n + j, n + j + 1);
                    }
                }
            }
        }
    }
}
