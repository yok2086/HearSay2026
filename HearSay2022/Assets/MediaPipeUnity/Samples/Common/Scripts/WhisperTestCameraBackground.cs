using System.Collections;
using UnityEngine;
using UnityEngine.UI;

#if UNITY_ANDROID && !UNITY_EDITOR
using UnityEngine.Android;
#endif

namespace HearSay
{
    // Camera background used only by the Whisper Test scene.
    // It uses Android's reported rotation/mirroring values and crops rather than stretches.
    public class WhisperTestCameraBackground : MonoBehaviour
    {
        private RawImage preview;
        private AspectRatioFitter previewFitter;
        private WebCamTexture cameraTexture;

        private void Start()
        {
            CreatePreview();
            StartCoroutine(StartCamera());
        }

        private IEnumerator StartCamera()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                var complete = false;
                var callbacks = new PermissionCallbacks();
                callbacks.PermissionGranted += _ => complete = true;
                callbacks.PermissionDenied += _ => complete = true;
                callbacks.PermissionDeniedAndDontAskAgain += _ => complete = true;
                Permission.RequestUserPermission(Permission.Camera, callbacks);
                yield return new WaitUntil(() => complete);
            }

            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                Debug.LogError("Whisper Test: camera permission was denied.");
                yield break;
            }
#endif

            var devices = WebCamTexture.devices;
            if (devices == null || devices.Length == 0)
            {
                Debug.LogError("Whisper Test: no phone camera was found.");
                yield break;
            }

            var device = devices[0];
            for (var i = 0; i < devices.Length; i++)
            {
                if (!devices[i].isFrontFacing)
                {
                    device = devices[i];
                    break;
                }
            }

            cameraTexture = new WebCamTexture(device.name, 1280, 720, 30);
            preview.texture = cameraTexture;
            preview.color = Color.clear;
            cameraTexture.Play();

            while (cameraTexture.width <= 16)
            {
                yield return null;
            }

            Debug.Log("Whisper Test: back camera started: " + device.name);
        }

        private void Update()
        {
            if (cameraTexture == null || cameraTexture.width <= 16)
            {
                return;
            }

            // Do not display Unity's default white RawImage before a camera frame exists.
            if (cameraTexture.didUpdateThisFrame)
            {
                preview.color = Color.white;
            }

            // WebCamTexture exposes the physical Android sensor orientation. Apply it once
            // to the UI preview, then preserve the image's aspect ratio while covering the screen.
            var quarterTurn = cameraTexture.videoRotationAngle == 90 || cameraTexture.videoRotationAngle == 270;
            var sourceAspect = (float)cameraTexture.width / cameraTexture.height;
            previewFitter.aspectRatio = quarterTurn ? 1f / sourceAspect : sourceAspect;
            preview.rectTransform.localEulerAngles = new Vector3(0f, 0f, -cameraTexture.videoRotationAngle);
            preview.rectTransform.localScale = new Vector3(1f, cameraTexture.videoVerticallyMirrored ? -1f : 1f, 1f);
        }

        private void OnDestroy()
        {
            if (cameraTexture != null)
            {
                cameraTexture.Stop();
            }
        }

        private void CreatePreview()
        {
            var canvasObject = new GameObject("Whisper Test Camera UI");
            var canvas = canvasObject.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = -100;
            canvasObject.AddComponent<CanvasScaler>();

            var previewObject = new GameObject("Phone Camera Preview");
            previewObject.transform.SetParent(canvasObject.transform, false);
            preview = previewObject.AddComponent<RawImage>();
            preview.color = Color.clear;
            previewFitter = previewObject.AddComponent<AspectRatioFitter>();
            previewFitter.aspectMode = AspectRatioFitter.AspectMode.EnvelopeParent;
            var rect = preview.rectTransform;
            rect.anchorMin = Vector2.zero;
            rect.anchorMax = Vector2.one;
            rect.offsetMin = Vector2.zero;
            rect.offsetMax = Vector2.zero;
        }
    }
}
