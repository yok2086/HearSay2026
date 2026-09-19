using UnityEngine;
using UnityEngine.UI;
using Mediapipe.Tasks.Vision.FaceLandmarker;
using Mediapipe.Tasks.Components.Containers;

namespace HearSay
{
    public class SpeakerMouthTest : MonoBehaviour
    {
        [Header("Test Settings")]
        [SerializeField] private float speakingThreshold = 0.05f;

        [Header("Camera Overlay")]
        [SerializeField] private float facePadding = 24f;
        [SerializeField] private bool showFaceBox = true;

        private int currentSpeaker = -1;
        private RectTransform speakerBox;
        private Text speakerStatusText;
        private readonly object overlayLock = new object();
        private bool overlayStateDirty;
        private bool pendingOverlayVisible;
        private Vector4 pendingFaceBounds;

        private void Awake()
        {
            SpeakerActivity.IsSpeaking = false;
            if (showFaceBox) CreateSpeakerBox();
            CreateSpeakerStatusLabel();
        }

        private void Update()
        {
            if (speakerStatusText != null)
            {
                speakerStatusText.text = SpeakerActivity.IsSpeaking
                    ? "SPEAKER DETECTED"
                    : "";
            }

            if (!showFaceBox) return;
            bool shouldShow;
            Vector4 faceBounds;

            lock (overlayLock)
            {
                if (!overlayStateDirty) return;
                overlayStateDirty = false;
                shouldShow = pendingOverlayVisible;
                faceBounds = pendingFaceBounds;
            }

            if (shouldShow) ApplySpeakerBox(faceBounds);
            else HideSpeakerBox();
        }

        public void ProcessFaceResult(FaceLandmarkerResult result)
        {
            if (result.faceLandmarks == null)
            {
                Debug.Log("No face landmark data.");
                QueueHiddenSpeakerBox();
                return;
            }

            if (result.faceBlendshapes == null ||
                result.faceBlendshapes.Count == 0)
            {
                Debug.Log("No face blendshape data.");
                QueueHiddenSpeakerBox();
                return;
            }

            int speakerFace = -1;
            float highestMouthMovement = 0f;

            for (int faceIndex = 0;
                 faceIndex < result.faceBlendshapes.Count;
                 faceIndex++)
            {
                Classifications classifications =
                    result.faceBlendshapes[faceIndex];

                if (classifications.categories == null)
                    continue;

                float mouthOpen = GetBlendshapeValue(
                    classifications.categories,
                    "jawOpen"
                );

                Debug.Log(
                    "Face " + faceIndex +
                    " | Mouth movement: " +
                    mouthOpen.ToString("F3")
                );

                if (mouthOpen > highestMouthMovement)
                {
                    highestMouthMovement = mouthOpen;
                    speakerFace = faceIndex;
                }
            }

            if (highestMouthMovement >= speakingThreshold)
            {
                SpeakerActivity.IsSpeaking = true;
                if (currentSpeaker != speakerFace)
                {
                    currentSpeaker = speakerFace;

                    Debug.Log(
                        ">>> CURRENT SPEAKER: FACE " +
                        currentSpeaker +
                        " <<<"
                    );
                }

                QueueSpeakerBox(result.faceLandmarks[speakerFace]);
            }
            else
            {
                if (currentSpeaker != -1)
                {
                    currentSpeaker = -1;

                    Debug.Log(">>> NO ONE SPEAKING <<<");
                }

                QueueHiddenSpeakerBox();
            }
        }

        private void OnDisable()
        {
            SpeakerActivity.IsSpeaking = false;
        }

        private void CreateSpeakerBox()
        {
            GameObject canvasObject = new GameObject("HearSay Speaker Overlay");
            Canvas canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 100;
            canvasObject.AddComponent<CanvasScaler>();
            canvasObject.AddComponent<GraphicRaycaster>();

            GameObject boxObject = new GameObject("Active Speaker Face");
            boxObject.transform.SetParent(canvasObject.transform, false);

            speakerBox = boxObject.AddComponent<RectTransform>();
            speakerBox.anchorMin = new Vector2(0.5f, 0.5f);
            speakerBox.anchorMax = new Vector2(0.5f, 0.5f);
            speakerBox.pivot = new Vector2(0.5f, 0.5f);

            Image image = boxObject.AddComponent<Image>();
            image.color = new Color(1f, 1f, 0f, 0f);

            Outline outline = boxObject.AddComponent<Outline>();
            outline.effectColor = Color.yellow;
            outline.effectDistance = new Vector2(5f, 5f);
            outline.useGraphicAlpha = false;

            HideSpeakerBox();
        }

        private void CreateSpeakerStatusLabel()
        {
            GameObject canvasObject = new GameObject("HearSay Speaker Status");
            Canvas canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = 200;
            canvasObject.AddComponent<CanvasScaler>();
            canvasObject.AddComponent<GraphicRaycaster>();

            GameObject labelObject = new GameObject("Speaker Detected Label");
            labelObject.transform.SetParent(canvasObject.transform, false);
            speakerStatusText = labelObject.AddComponent<Text>();
            speakerStatusText.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            speakerStatusText.fontSize = 34;
            speakerStatusText.fontStyle = FontStyle.Bold;
            speakerStatusText.alignment = TextAnchor.UpperCenter;
            speakerStatusText.color = Color.yellow;

            RectTransform labelRect = speakerStatusText.rectTransform;
            labelRect.anchorMin = new Vector2(0.5f, 1f);
            labelRect.anchorMax = new Vector2(0.5f, 1f);
            labelRect.pivot = new Vector2(0.5f, 1f);
            labelRect.anchoredPosition = new Vector2(0f, -80f);
            labelRect.sizeDelta = new Vector2(700f, 80f);
        }

        private void QueueSpeakerBox(NormalizedLandmarks face)
        {
            if (face.landmarks == null || face.landmarks.Count == 0)
            {
                return;
            }

            float minX = 1f;
            float maxX = 0f;
            float minY = 1f;
            float maxY = 0f;

            for (int i = 0; i < face.landmarks.Count; i++)
            {
                NormalizedLandmark landmark = face.landmarks[i];
                if (landmark.x < minX) minX = landmark.x;
                if (landmark.x > maxX) maxX = landmark.x;
                if (landmark.y < minY) minY = landmark.y;
                if (landmark.y > maxY) maxY = landmark.y;
            }

            lock (overlayLock)
            {
                pendingFaceBounds = new Vector4(minX, maxX, minY, maxY);
                pendingOverlayVisible = true;
                overlayStateDirty = true;
            }
        }

        private void QueueHiddenSpeakerBox()
        {
            lock (overlayLock)
            {
                pendingOverlayVisible = false;
                overlayStateDirty = true;
            }
        }

        private void ApplySpeakerBox(Vector4 faceBounds)
        {
            if (speakerBox == null) return;

            float centerX = (faceBounds.x + faceBounds.y) * 0.5f;
            float centerY = (faceBounds.z + faceBounds.w) * 0.5f;

            speakerBox.anchoredPosition = new Vector2(
                (centerX - 0.5f) * Screen.width,
                (0.5f - centerY) * Screen.height
            );
            speakerBox.sizeDelta = new Vector2(
                (faceBounds.y - faceBounds.x) * Screen.width + facePadding,
                (faceBounds.w - faceBounds.z) * Screen.height + facePadding
            );
            speakerBox.gameObject.SetActive(true);
        }

        private void HideSpeakerBox()
        {
            if (speakerBox != null)
            {
                speakerBox.gameObject.SetActive(false);
            }
        }

        private float GetBlendshapeValue(
            System.Collections.Generic.IReadOnlyList<Category> categories,
            string name
        )
        {
            if (categories == null)
                return 0f;

            for (int i = 0; i < categories.Count; i++)
            {
                if (categories[i].categoryName == name)
                {
                    return categories[i].score;
                }
            }

            return 0f;
        }
    }

    public static class SpeakerActivity
    {
        public static volatile bool IsSpeaking;
    }
}
