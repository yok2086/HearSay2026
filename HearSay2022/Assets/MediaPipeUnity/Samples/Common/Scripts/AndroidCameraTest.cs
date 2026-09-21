using System.Collections;
using UnityEngine;
using UnityEngine.UI;

#if UNITY_ANDROID && !UNITY_EDITOR
using UnityEngine.Android;
#endif

namespace HearSay
{
    // Deliberately does not use MediaPipe. This isolates the Android camera itself.
    public class AndroidCameraTest : MonoBehaviour
    {
        private RawImage preview;
        private Text status;
        private WebCamTexture cameraTexture;

        private void Start()
        {
            CreateUi();
            StartCoroutine(StartCamera());
        }

        private IEnumerator StartCamera()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                status.text = "Requesting phone camera permission…";
                var requestFinished = false;
                var callbacks = new PermissionCallbacks();
                callbacks.PermissionGranted += _ => requestFinished = true;
                callbacks.PermissionDenied += _ => requestFinished = true;
                callbacks.PermissionDeniedAndDontAskAgain += _ => requestFinished = true;
                Permission.RequestUserPermission(Permission.Camera, callbacks);
                yield return new WaitUntil(() => requestFinished);
            }

            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                status.text = "CAMERA PERMISSION DENIED";
                yield break;
            }
#endif

            var devices = WebCamTexture.devices;
            if (devices == null || devices.Length == 0)
            {
                status.text = "NO PHONE CAMERA FOUND";
                yield break;
            }

            var selectedDevice = devices[0];
            for (var i = 0; i < devices.Length; i++)
            {
                if (!devices[i].isFrontFacing)
                {
                    selectedDevice = devices[i];
                    break;
                }
            }

            cameraTexture = new WebCamTexture(selectedDevice.name, 1280, 720, 30);
            preview.texture = cameraTexture;
            cameraTexture.Play();
            status.text = "Opening: " + selectedDevice.name;

            var elapsed = 0f;
            var receivedFrame = false;
            while (!receivedFrame && elapsed < 10f)
            {
                receivedFrame = cameraTexture.didUpdateThisFrame;
                elapsed += Time.unscaledDeltaTime;
                yield return null;
            }

            status.text = receivedFrame
                ? "FRAMES RECEIVED: " + selectedDevice.name
                : "NO VIDEO FRAMES: " + selectedDevice.name;
        }

        private void OnDestroy()
        {
            if (cameraTexture != null)
            {
                cameraTexture.Stop();
            }
        }

        private void CreateUi()
        {
            var canvasObject = new GameObject("Android Camera Test UI");
            var canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvasObject.AddComponent<CanvasScaler>();
            canvasObject.AddComponent<GraphicRaycaster>();

            var previewObject = new GameObject("Phone Camera Preview");
            previewObject.transform.SetParent(canvasObject.transform, false);
            preview = previewObject.AddComponent<RawImage>();
            preview.color = Color.white;
            var previewRect = preview.rectTransform;
            previewRect.anchorMin = Vector2.zero;
            previewRect.anchorMax = Vector2.one;
            previewRect.offsetMin = Vector2.zero;
            previewRect.offsetMax = Vector2.zero;

            var statusObject = new GameObject("Camera Status");
            statusObject.transform.SetParent(canvasObject.transform, false);
            status = statusObject.AddComponent<Text>();
            status.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            status.fontSize = 32;
            status.fontStyle = FontStyle.Bold;
            status.alignment = TextAnchor.UpperCenter;
            status.color = Color.white;
            var statusRect = status.rectTransform;
            statusRect.anchorMin = new Vector2(0.5f, 1f);
            statusRect.anchorMax = new Vector2(0.5f, 1f);
            statusRect.pivot = new Vector2(0.5f, 1f);
            statusRect.anchoredPosition = new Vector2(0f, -40f);
            statusRect.sizeDelta = new Vector2(1100f, 80f);
        }
    }
}
