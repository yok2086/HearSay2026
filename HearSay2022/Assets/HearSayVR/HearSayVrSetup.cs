using UnityEngine;
using UnityEngine.SceneManagement;

namespace HearSay
{
    // Camera-free setup: QR scanning cannot contend with MediaPipe capture here.
    public sealed class HearSayVrSetup : MonoBehaviour
    {
        private Google.XR.Cardboard.XRLoader calibrationLoader;
        private string address;
        private string message = "";
        private void Awake()
        {
            address = PlayerPrefs.GetString("HearSay.Server", "http://127.0.0.1:8080/status");
            var camera = gameObject.AddComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(0.04f, 0.06f, 0.1f);
            camera.cullingMask = 0;
        }

        private void OnDestroy()
        {
            if (calibrationLoader == null) return;
            calibrationLoader.Deinitialize();
            Destroy(calibrationLoader);
        }

        private void OnGUI()
        {
            float scale = Mathf.Min(Screen.width / 1000f, Screen.height / 650f);
            var previous = GUI.matrix;
            GUI.matrix = Matrix4x4.TRS(Vector3.zero, Quaternion.identity, Vector3.one * scale);
            GUI.Label(new Rect(60, 50, 900, 70), "HearSay VR test — select before inserting the phone");
            GUI.Label(new Rect(60, 110, 880, 40), "Camera passthrough is monoscopic. Keep the rear camera uncovered. Test seated.");
            address = GUI.TextField(new Rect(60, 165, 880, 45), address);
            if (GUI.Button(new Rect(60, 230, 880, 70), "1. Scan headset QR code"))
            {
#if UNITY_ANDROID && !UNITY_EDITOR
                if (calibrationLoader == null)
                {
                    calibrationLoader = ScriptableObject.CreateInstance<Google.XR.Cardboard.XRLoader>();
                    calibrationLoader.Initialize(); // No Start: keep the setup screen flat.
                }
                Google.XR.Cardboard.Api.ScanDeviceParams();
#else
                Debug.Log("Viewer QR scanning is available in the Android build.");
#endif
            }
            if (GUI.Button(new Rect(60, 330, 880, 70), "2. Start HearSay VR Test"))
            {
                if (System.Uri.TryCreate(address.Trim(), System.UriKind.Absolute, out var uri) &&
                    (uri.Scheme == "http" || uri.Scheme == "https"))
                {
                    PlayerPrefs.SetString("HearSay.Server", new System.Uri(uri, "/status").AbsoluteUri);
                    PlayerPrefs.SetInt("HearSay.VRMode", 1);
                    PlayerPrefs.Save();
                    // Use the real Home scene so its layout and flow stay
                    // identical; HearSayRoles adds the Cardboard adapter there.
                    SceneManager.LoadScene("HearSay Start");
                }
                else message = "Enter a valid http:// server address.";
            }
            if (GUI.Button(new Rect(60, 430, 880, 70), "Open normal HearSay instead"))
            {
                PlayerPrefs.SetInt("HearSay.VRMode", 0);
                PlayerPrefs.Save();
                SceneManager.LoadScene("HearSay Start");
            }
            GUI.Label(new Rect(60, 530, 880, 60), message);
            GUI.matrix = previous;
        }
    }
}
