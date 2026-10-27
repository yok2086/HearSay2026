using UnityEngine;

namespace HearSay
{
    // Fallback for Cardboard viewers when the XR display is stereo but does not
    // provide a camera pose. It uses the phone's built-in gyroscope only in VR.
    public sealed class HearSayVrGyroFallback : MonoBehaviour
    {
        private Quaternion calibration;
        private Quaternion manualRotation;
        private bool manualReady;

        private void Start()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            Input.gyro.enabled = true;
            Input.gyro.updateInterval = 0.01f;
            calibration = Quaternion.Inverse(Convert(Input.gyro.attitude));
            manualRotation = Quaternion.identity;
            manualReady = true;
            Application.onBeforeRender += ApplyPose;
#endif
        }

        private void LateUpdate()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            ApplyPose();
#endif
        }

        private void ApplyPose()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            // Cardboard normally supplies the camera pose. Some phones/builds
            // start the stereo display but do not write that pose to Camera.
            // Integrate rotationRate as a reliable fallback so phone tilt still
            // moves the view in the headset.
            if (manualReady)
            {
                Vector3 rate = Input.gyro.rotationRateUnbiased;
                float angle = rate.magnitude * Time.unscaledDeltaTime * Mathf.Rad2Deg;
                if (angle > 0.0001f)
                    manualRotation = Quaternion.AngleAxis(angle, new Vector3(-rate.x, -rate.y, rate.z)) * manualRotation;
                transform.localRotation = manualRotation;
            }
            else
            {
                transform.localRotation = calibration * Convert(Input.gyro.attitude);
            }
#endif
        }

        private void OnDestroy()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            Application.onBeforeRender -= ApplyPose;
#endif
        }

        private static Quaternion Convert(Quaternion q)
        {
            return new Quaternion(q.x, q.y, -q.z, -q.w);
        }
    }
}
