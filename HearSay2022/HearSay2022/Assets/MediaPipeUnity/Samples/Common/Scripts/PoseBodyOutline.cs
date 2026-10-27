using UnityEngine;
using UnityEngine.UI;
using Mediapipe.Tasks.Components.Containers;
using Mediapipe.Tasks.Vision.PoseLandmarker;

namespace HearSay
{
    // Draws a simple yellow body outline from MediaPipe pose landmarks.
    public class PoseBodyOutline : MonoBehaviour
    {
        [SerializeField] private float lineThickness = 10f;

        private static readonly int[,] BodyConnections =
        {
            { 11, 12 }, { 11, 13 }, { 13, 15 }, { 12, 14 }, { 14, 16 },
            { 11, 23 }, { 12, 24 }, { 23, 24 }, { 23, 25 }, { 25, 27 },
            { 24, 26 }, { 26, 28 }
        };

        private readonly object poseLock = new object();
        private readonly Vector2[] pendingPoints = new Vector2[33];
        private bool poseDirty;
        private bool hasPose;
        private RectTransform[] lines;

        private void Awake()
        {
            CreateOutlineCanvas();
        }

        public void ProcessPoseResult(PoseLandmarkerResult result)
        {
            if (result.poseLandmarks == null || result.poseLandmarks.Count == 0 ||
                result.poseLandmarks[0].landmarks == null ||
                result.poseLandmarks[0].landmarks.Count < pendingPoints.Length)
            {
                QueuePose(false);
                return;
            }

            NormalizedLandmarks pose = result.poseLandmarks[0];
            lock (poseLock)
            {
                for (int i = 0; i < pendingPoints.Length; i++)
                {
                    NormalizedLandmark landmark = pose.landmarks[i];
                    pendingPoints[i] = new Vector2(landmark.x, landmark.y);
                }

                hasPose = true;
                poseDirty = true;
            }
        }

        private void Update()
        {
            Vector2[] points = null;

            lock (poseLock)
            {
                if (!poseDirty) return;
                poseDirty = false;

                if (hasPose)
                {
                    points = (Vector2[])pendingPoints.Clone();
                }
            }

            if (points == null)
            {
                SetLinesActive(false);
                return;
            }

            for (int i = 0; i < BodyConnections.GetLength(0); i++)
            {
                Vector2 start = ToScreenPoint(points[BodyConnections[i, 0]]);
                Vector2 end = ToScreenPoint(points[BodyConnections[i, 1]]);
                DrawLine(lines[i], start, end);
            }
        }

        private void QueuePose(bool visible)
        {
            lock (poseLock)
            {
                hasPose = visible;
                poseDirty = true;
            }
        }

        private void CreateOutlineCanvas()
        {
            GameObject canvasObject = new GameObject("HearSay Body Outline");
            Canvas canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 100;
            canvasObject.AddComponent<CanvasScaler>();
            canvasObject.AddComponent<GraphicRaycaster>();

            lines = new RectTransform[BodyConnections.GetLength(0)];
            for (int i = 0; i < lines.Length; i++)
            {
                GameObject lineObject = new GameObject("Yellow Body Line " + i);
                lineObject.transform.SetParent(canvasObject.transform, false);
                Image image = lineObject.AddComponent<Image>();
                image.color = Color.yellow;
                lines[i] = lineObject.GetComponent<RectTransform>();
                lines[i].pivot = new Vector2(0.5f, 0.5f);
            }

            SetLinesActive(false);
        }

        private static Vector2 ToScreenPoint(Vector2 normalizedPoint)
        {
            return new Vector2(
                (normalizedPoint.x - 0.5f) * Screen.width,
                (0.5f - normalizedPoint.y) * Screen.height
            );
        }

        private void DrawLine(RectTransform line, Vector2 start, Vector2 end)
        {
            Vector2 delta = end - start;
            line.gameObject.SetActive(true);
            line.anchoredPosition = (start + end) * 0.5f;
            line.sizeDelta = new Vector2(delta.magnitude, lineThickness);
            line.localRotation = Quaternion.Euler(0f, 0f,
                Mathf.Atan2(delta.y, delta.x) * Mathf.Rad2Deg);
        }

        private void SetLinesActive(bool visible)
        {
            if (lines == null) return;
            for (int i = 0; i < lines.Length; i++)
            {
                lines[i].gameObject.SetActive(visible);
            }
        }
    }
}
