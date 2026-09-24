using Mediapipe.Unity;
using UnityEngine;

namespace HearSay
{
    [DefaultExecutionOrder(-10000)]
    public class HearSayDemoCameraPreference : MonoBehaviour
    {
        private void Awake()
        {
            // The deaf user looks outward at the speaker through the rear camera.
            WebCamSource.PreferFrontCamera = false;
        }

        private void OnDestroy()
        {
            WebCamSource.PreferFrontCamera = false;
        }
    }
}
