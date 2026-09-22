using Mediapipe.Unity;
using UnityEngine;

namespace HearSay
{
    [DefaultExecutionOrder(-10000)]
    public class HearSayDemoCameraPreference : MonoBehaviour
    {
        private void Awake()
        {
            WebCamSource.PreferFrontCamera = true;
        }

        private void OnDestroy()
        {
            WebCamSource.PreferFrontCamera = false;
        }
    }
}
