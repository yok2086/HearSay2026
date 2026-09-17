
using UnityEngine;
using Mediapipe.Tasks.Vision.FaceLandmarker;
using Mediapipe.Tasks.Components.Containers;

namespace HearSay
{
    public class SpeakerMouthTest : MonoBehaviour
    {
        [Header("Test Settings")]
        [SerializeField] private float speakingThreshold = 0.15f;

        public void ProcessFaceResult(FaceLandmarkerResult result)
        {
            if (result.faceBlendshapes == null ||
                result.faceBlendshapes.Count == 0)
            {
                Debug.Log("No face blendshape data.");
                return;
            }

            for (int faceIndex = 0; faceIndex < result.faceBlendshapes.Count; faceIndex++)
            {
                Classifications classifications =
                    result.faceBlendshapes[faceIndex];

                var categories = classifications.categories;

                float mouthOpen = GetBlendshapeValue(
                    categories,
                    "jawOpen"
                );

                Debug.Log(
                    "Face " + faceIndex +
                    " | Mouth movement: " +
                    mouthOpen.ToString("F3")
                );

                if (mouthOpen > speakingThreshold)
                {
                    Debug.Log(
                        ">>> FACE " + faceIndex +
                        " IS SPEAKING <<<"
                    );
                }
            }
        }

        private float GetBlendshapeValue(
            System.Collections.Generic.IReadOnlyList<Category> categories,
            string name
        )
        {
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