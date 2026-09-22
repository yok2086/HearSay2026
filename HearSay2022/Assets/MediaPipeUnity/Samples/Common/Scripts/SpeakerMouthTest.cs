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
        [SerializeField] private int framesToConfirmSpeaker = 1;
        [SerializeField] private float minimumFaceWidth = 0f;
        [Tooltip("How far a face must be from the locked speaker before it counts as another person.")]
        [SerializeField] private float newSpeakerMinimumDistance = 0.14f;
        [Tooltip("How far the locked face may move between detections before it is considered lost.")]
        [SerializeField] private float lockedSpeakerLostDistance = 0.30f;
        [SerializeField] private int framesToLoseSpeaker = 5;
        [SerializeField] private bool requireRecentVoiceActivity;
        [SerializeField] private float voiceActivityHoldSeconds = 0.65f;

        [Header("Camera Overlay")]
        [SerializeField] private float facePadding = 24f;
        [SerializeField] private bool showFaceBox = true;

        private int currentSpeaker = -1;
        private Vector2 currentSpeakerCenter;
        private int candidateSpeaker = -1;
        private int candidateFrames;
        private int missingLockedSpeakerFrames;
        private RectTransform[] faceOutlineSegments;
        private Text speakerStatusText;
        private Mediapipe.Unity.Screen cameraPreviewScreen;
        private RectTransform faceOutlineParent;
        private readonly object overlayLock = new object();
        private bool overlayStateDirty;
        private bool pendingOverlayVisible;
        private Vector4 pendingFaceBounds;
        private Vector2[] pendingFaceContour;
        private Vector2 pendingMouthPosition;

        // MediaPipe's standard face-oval contour, ordered clockwise around the face.
        private static readonly int[] FaceOvalLandmarkIndices =
        {
            10, 338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288,
            397, 365, 379, 378, 400, 377, 152, 148, 176, 149, 150, 136,
            172, 58, 132, 93, 234, 127, 162, 21, 54, 103, 67, 109,
        };

        private void Awake()
        {
            SpeakerActivity.IsSpeaking = false;
            SpeakerActivity.IsSpeakerCurrentlyActive = false;
            SpeakerActivity.HasMouthPosition = false;
            cameraPreviewScreen = FindFirstObjectByType<Mediapipe.Unity.Screen>();
            if (showFaceBox) CreateSpeakerBox();
            CreateSpeakerStatusLabel();
        }

        private void Update()
        {
            if (speakerStatusText != null)
            {
                speakerStatusText.text = SpeakerActivity.IsSpeaking
                    ? "SPEAKER DETECTED"
                    : "NO SPEAKER DETECTED";
            }

            bool shouldShow;
            Vector4 faceBounds;
            Vector2[] faceContour;
            Vector2 mouthPosition;

            lock (overlayLock)
            {
                if (!overlayStateDirty) return;
                overlayStateDirty = false;
                shouldShow = pendingOverlayVisible;
                faceBounds = pendingFaceBounds;
                faceContour = pendingFaceContour;
                mouthPosition = pendingMouthPosition;
            }

            if (shouldShow && SpeakerActivity.IsSpeaking)
            {
                SpeakerActivity.CurrentFaceBounds = faceBounds;
                SpeakerActivity.HasFaceBounds = true;
                SpeakerActivity.CurrentMouthPosition = mouthPosition;
                SpeakerActivity.HasMouthPosition = true;
                if (showFaceBox) ApplyFaceOutline(faceContour);
            }
            else
            {
                SpeakerActivity.HasFaceBounds = false;
                SpeakerActivity.HasMouthPosition = false;
                if (showFaceBox) HideFaceOutline();
            }
        }

        public void ProcessFaceResult(FaceLandmarkerResult result)
        {
            if (result.faceLandmarks == null || result.faceLandmarks.Count == 0)
            {
                Debug.Log("No face landmark data.");
                MarkLockedSpeakerMissing();
                return;
            }

            if (result.faceBlendshapes == null ||
                result.faceBlendshapes.Count == 0)
            {
                Debug.Log("No face blendshape data.");
                MarkLockedSpeakerMissing();
                return;
            }

            // After a speaker is locked, keep refreshing that face's landmark
            // contour every frame. Mouth movement is only needed to select/switch.
            if (RefreshLockedSpeakerOutline(result.faceLandmarks))
            {
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

                if (faceIndex >= result.faceLandmarks.Count ||
                    GetFaceWidth(result.faceLandmarks[faceIndex]) < minimumFaceWidth)
                {
                    continue;
                }

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

            SpeakerActivity.CurrentMouthMovement = highestMouthMovement;

            if (highestMouthMovement >= speakingThreshold)
            {
                Vector2 movingFaceCenter = GetFaceCenter(result.faceLandmarks[speakerFace]);
                bool differentFromLockedSpeaker = currentSpeaker >= 0 &&
                    Vector2.Distance(movingFaceCenter, currentSpeakerCenter) >= newSpeakerMinimumDistance;

                // Initial lock: mouth movement alone. Switching: another face must
                // move its mouth and the microphone must have recent voice activity.
                if (differentFromLockedSpeaker && requireRecentVoiceActivity &&
                    !HearSayAudioActivity.VoiceDetectedRecently(voiceActivityHoldSeconds))
                {
                    ClearSpeakerCandidate();
                    HideOverlayOnlyWhenNoSpeakerIsLocked();
                    return;
                }

                if (candidateSpeaker == speakerFace)
                {
                    candidateFrames++;
                }
                else
                {
                    candidateSpeaker = speakerFace;
                    candidateFrames = 1;
                }

                if (candidateFrames < framesToConfirmSpeaker)
                {
                    SpeakerActivity.IsSpeakerCurrentlyActive = false;
                    // Keep displaying the previously confirmed speaker while a
                    // possible new speaker is still being verified.
                    HideOverlayOnlyWhenNoSpeakerIsLocked();
                    return;
                }

                SpeakerActivity.IsSpeaking = true;
                SpeakerActivity.IsSpeakerCurrentlyActive = true;
                if (currentSpeaker == -1 || differentFromLockedSpeaker)
                {
                    Debug.Log(
                        currentSpeaker == -1
                            ? ">>> FIRST SPEAKER DETECTED: FACE " + speakerFace + " <<<"
                            : ">>> NEW SPEAKER DETECTED: FACE " + speakerFace + " <<<"
                    );
                }
                currentSpeaker = speakerFace;
                currentSpeakerCenter = movingFaceCenter;

                QueueSpeakerBox(result.faceLandmarks[speakerFace]);
            }
            else
            {
                // A closed mouth means the current speaker paused. It does not
                // mean they stopped being the selected speaker.
                ClearSpeakerCandidate();
                HideOverlayOnlyWhenNoSpeakerIsLocked();
            }
        }

        private void OnDisable()
        {
            SpeakerActivity.IsSpeaking = false;
            SpeakerActivity.IsSpeakerCurrentlyActive = false;
            SpeakerActivity.HasFaceBounds = false;
            SpeakerActivity.HasMouthPosition = false;
        }

        private void CreateSpeakerBox()
        {
            if (cameraPreviewScreen == null)
            {
                cameraPreviewScreen = FindFirstObjectByType<Mediapipe.Unity.Screen>();
            }

            Transform parent;
            if (cameraPreviewScreen != null)
            {
                // Parent the landmark contour to the actual camera RawImage.
                // This makes it rotate/crop exactly with the camera feed.
                faceOutlineParent = cameraPreviewScreen.overlayRectTransform;
                parent = faceOutlineParent;
            }
            else
            {
                GameObject canvasObject = new GameObject("HearSay Speaker Overlay");
                Canvas canvas = canvasObject.AddComponent<Canvas>();
                canvas.renderMode = RenderMode.ScreenSpaceOverlay;
                canvas.sortingOrder = 100;
                canvasObject.AddComponent<CanvasScaler>();
                canvasObject.AddComponent<GraphicRaycaster>();
                parent = canvasObject.transform;
            }

            faceOutlineSegments = new RectTransform[FaceOvalLandmarkIndices.Length];
            for (int i = 0; i < faceOutlineSegments.Length; i++)
            {
                GameObject segmentObject = new GameObject("Active Speaker Face Contour");
                segmentObject.transform.SetParent(parent, false);

                RectTransform segment = segmentObject.AddComponent<RectTransform>();
                segment.anchorMin = new Vector2(0.5f, 0.5f);
                segment.anchorMax = new Vector2(0.5f, 0.5f);
                segment.pivot = new Vector2(0.5f, 0.5f);

                Image image = segmentObject.AddComponent<Image>();
                image.color = Color.yellow;
                image.raycastTarget = false;
                faceOutlineSegments[i] = segment;
            }

            HideFaceOutline();
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
                pendingFaceContour = GetFaceOvalContour(face);
                pendingMouthPosition = GetMouthCenter(face);
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

        private void ClearSpeakerCandidate()
        {
            SpeakerActivity.IsSpeakerCurrentlyActive = false;
            SpeakerActivity.HasMouthPosition = false;
            candidateSpeaker = -1;
            candidateFrames = 0;
        }

        private void HideOverlayOnlyWhenNoSpeakerIsLocked()
        {
            if (!SpeakerActivity.IsSpeaking)
            {
                QueueHiddenSpeakerBox();
            }
        }

        private static float GetFaceWidth(NormalizedLandmarks face)
        {
            if (face.landmarks == null || face.landmarks.Count == 0)
            {
                return 0f;
            }

            float minX = 1f;
            float maxX = 0f;
            for (int i = 0; i < face.landmarks.Count; i++)
            {
                minX = Mathf.Min(minX, face.landmarks[i].x);
                maxX = Mathf.Max(maxX, face.landmarks[i].x);
            }
            return maxX - minX;
        }

        private static Vector2 GetFaceCenter(NormalizedLandmarks face)
        {
            if (face.landmarks == null || face.landmarks.Count == 0)
            {
                return new Vector2(0.5f, 0.5f);
            }

            float minX = 1f;
            float maxX = 0f;
            float minY = 1f;
            float maxY = 0f;
            for (int i = 0; i < face.landmarks.Count; i++)
            {
                NormalizedLandmark landmark = face.landmarks[i];
                minX = Mathf.Min(minX, landmark.x);
                maxX = Mathf.Max(maxX, landmark.x);
                minY = Mathf.Min(minY, landmark.y);
                maxY = Mathf.Max(maxY, landmark.y);
            }
            return new Vector2((minX + maxX) * 0.5f, (minY + maxY) * 0.5f);
        }

        private bool RefreshLockedSpeakerOutline(System.Collections.Generic.IReadOnlyList<NormalizedLandmarks> faces)
        {
            if (!SpeakerActivity.IsSpeaking || faces == null || faces.Count == 0)
            {
                return false;
            }

            int closestFace = -1;
            float closestDistance = float.MaxValue;
            for (int i = 0; i < faces.Count; i++)
            {
                if (GetFaceWidth(faces[i]) < minimumFaceWidth)
                {
                    continue;
                }

                float distance = Vector2.Distance(GetFaceCenter(faces[i]), currentSpeakerCenter);
                if (distance < closestDistance)
                {
                    closestDistance = distance;
                    closestFace = i;
                }
            }

            if (closestFace >= 0)
            {
                if (closestDistance > lockedSpeakerLostDistance)
                {
                    MarkLockedSpeakerMissing();
                    return true;
                }

                missingLockedSpeakerFrames = 0;
                currentSpeaker = closestFace;
                currentSpeakerCenter = GetFaceCenter(faces[closestFace]);
                QueueSpeakerBox(faces[closestFace]);
            }
            else
            {
                MarkLockedSpeakerMissing();
                return true;
            }

            return false;
        }

        private void MarkLockedSpeakerMissing()
        {
            if (!SpeakerActivity.IsSpeaking)
            {
                ClearSpeakerCandidate();
                QueueHiddenSpeakerBox();
                return;
            }

            missingLockedSpeakerFrames++;
            if (missingLockedSpeakerFrames < Mathf.Max(1, framesToLoseSpeaker))
            {
                return;
            }

            SpeakerActivity.IsSpeaking = false;
            SpeakerActivity.IsSpeakerCurrentlyActive = false;
            SpeakerActivity.HasFaceBounds = false;
            SpeakerActivity.HasMouthPosition = false;
            currentSpeaker = -1;
            candidateSpeaker = -1;
            candidateFrames = 0;
            missingLockedSpeakerFrames = 0;
            QueueHiddenSpeakerBox();
        }

        private Vector2[] GetFaceOvalContour(NormalizedLandmarks face)
        {
            var contour = new Vector2[FaceOvalLandmarkIndices.Length];
            for (int i = 0; i < FaceOvalLandmarkIndices.Length; i++)
            {
                int landmarkIndex = FaceOvalLandmarkIndices[i];
                if (landmarkIndex >= face.landmarks.Count)
                {
                    return null;
                }

                NormalizedLandmark landmark = face.landmarks[landmarkIndex];
                contour[i] = new Vector2(landmark.x, landmark.y);
            }
            return contour;
        }

        private static Vector2 GetMouthCenter(NormalizedLandmarks face)
        {
            // 13 and 14 are MediaPipe's upper- and lower-inner-lip landmarks.
            const int upperLip = 13;
            const int lowerLip = 14;
            if (face.landmarks.Count <= lowerLip)
            {
                return new Vector2(0.5f, 0.5f);
            }

            NormalizedLandmark upper = face.landmarks[upperLip];
            NormalizedLandmark lower = face.landmarks[lowerLip];
            return new Vector2((upper.x + lower.x) * 0.5f, (upper.y + lower.y) * 0.5f);
        }

        private void ApplyFaceOutline(Vector2[] contour)
        {
            if (faceOutlineSegments == null || contour == null || contour.Length != faceOutlineSegments.Length)
            {
                return;
            }

            for (int i = 0; i < faceOutlineSegments.Length; i++)
            {
                Vector2 from = ToScreenPoint(contour[i]);
                Vector2 to = ToScreenPoint(contour[(i + 1) % contour.Length]);
                Vector2 delta = to - from;
                RectTransform segment = faceOutlineSegments[i];
                segment.anchoredPosition = (from + to) * 0.5f;
                segment.sizeDelta = new Vector2(delta.magnitude, 5f);
                segment.localRotation = Quaternion.Euler(0f, 0f, Mathf.Atan2(delta.y, delta.x) * Mathf.Rad2Deg);
                segment.gameObject.SetActive(true);
            }
        }

        private Vector2 ToScreenPoint(Vector2 normalizedPoint)
        {
            if (faceOutlineParent != null)
            {
                return new Vector2(
                    (normalizedPoint.x - 0.5f) * faceOutlineParent.rect.width,
                    (0.5f - normalizedPoint.y) * faceOutlineParent.rect.height
                );
            }

            return new Vector2(
                (normalizedPoint.x - 0.5f) * Screen.width,
                (0.5f - normalizedPoint.y) * Screen.height
            );
        }

        private void HideFaceOutline()
        {
            if (faceOutlineSegments == null)
            {
                return;
            }

            foreach (RectTransform segment in faceOutlineSegments)
            {
                if (segment != null)
                {
                    segment.gameObject.SetActive(false);
                }
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
        // IsSpeaking means a face is locked as the selected speaker.
        public static volatile bool IsSpeaking;
        // IsSpeakerCurrentlyActive means that the selected speaker is talking now.
        public static volatile bool IsSpeakerCurrentlyActive;
        public static bool HasFaceBounds;
        public static Vector4 CurrentFaceBounds;
        public static bool HasMouthPosition;
        public static Vector2 CurrentMouthPosition;
        public static float CurrentMouthMovement;
    }
}
