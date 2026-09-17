using UnityEngine;
using Mediapipe.Tasks.Vision.FaceLandmarker;
using Mediapipe.Tasks.Components.Containers;

namespace HearSay
{
    public class SpeakerMouthTest : MonoBehaviour
    {
        [Header("Test Settings")]
        [SerializeField] private float speakingThreshold = 0.15f;

        private int currentSpeaker = -1;

        public void ProcessFaceResult(FaceLandmarkerResult result)
        {
            if (result.faceLandmarks == null)
            {
                Debug.Log("No face landmark data.");
                return;
            }

            if (result.faceBlendshapes == null ||
                result.faceBlendshapes.Count == 0)
            {
                Debug.Log("No face blendshape data.");
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
                if (currentSpeaker != speakerFace)
                {
                    currentSpeaker = speakerFace;

                    Debug.Log(
                        ">>> CURRENT SPEAKER: FACE " +
                        currentSpeaker +
                        " <<<"
                    );
                }
            }
            else
            {
                if (currentSpeaker != -1)
                {
                    currentSpeaker = -1;

                    Debug.Log(">>> NO ONE SPEAKING <<<");
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
}