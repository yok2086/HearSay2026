using System.Collections;
using UnityEngine;
using UnityEngine.UI;
#if UNITY_ANDROID && !UNITY_EDITOR
using UnityEngine.Android;
#endif

namespace HearSay
{
    public class HearSayPartnerCamera : MonoBehaviour
    {
        public string Status { get; private set; } = "Starting camera…";
        private WebCamTexture cameraTexture;
        public WebCamTexture CurrentTexture => cameraTexture;
        private GameObject canvasObject;
        private RawImage preview;

        private IEnumerator Start()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                bool completed = false;
                var callbacks = new PermissionCallbacks();
                callbacks.PermissionGranted += _ => completed = true;
                callbacks.PermissionDenied += _ => completed = true;
                callbacks.PermissionDeniedAndDontAskAgain += _ => completed = true;
                Permission.RequestUserPermission(Permission.Camera, callbacks);
                yield return new WaitUntil(() => completed);
            }
            if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
            {
                Status = "Camera permission denied — enable Camera in app settings.";
                yield break;
            }
#endif
            var devices = WebCamTexture.devices;
            if (devices.Length == 0) { Status = "No camera found."; yield break; }
            var device = devices[0];
            foreach (var candidate in devices)
                if (!candidate.isFrontFacing) { device = candidate; break; }

            canvasObject = new GameObject("Partner surroundings camera", typeof(Canvas));
            var canvas = canvasObject.GetComponent<Canvas>();
            canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            canvas.sortingOrder = -100;
            preview = new GameObject("Back camera", typeof(RectTransform), typeof(RawImage)).GetComponent<RawImage>();
            preview.transform.SetParent(canvasObject.transform, false);
            preview.raycastTarget = false;
            preview.color = Color.clear;
            preview.rectTransform.anchorMin = preview.rectTransform.anchorMax = new Vector2(0.5f, 0.5f);
            cameraTexture = new WebCamTexture(device.name, 1280, 720, 30);
            preview.texture = cameraTexture;
            cameraTexture.Play();
            float deadline = Time.realtimeSinceStartup + 10f;
            while (cameraTexture.width <= 16 && Time.realtimeSinceStartup < deadline) yield return null;
            Status = cameraTexture.width > 16 ? "" : "Camera has not delivered frames. Check camera access.";
        }

        private void Update()
        {
            if (cameraTexture == null || preview == null || cameraTexture.width <= 16) return;
            if (cameraTexture.didUpdateThisFrame) { preview.color = Color.white; Status = ""; }
            int angle = cameraTexture.videoRotationAngle;
            bool quarterTurn = angle == 90 || angle == 270;
            float displayWidth = quarterTurn ? cameraTexture.height : cameraTexture.width;
            float displayHeight = quarterTurn ? cameraTexture.width : cameraTexture.height;
            float scale = Mathf.Max(Screen.width / displayWidth, Screen.height / displayHeight);
            // Size in sensor coordinates, then rotate once. Crop without stretching.
            preview.rectTransform.sizeDelta = new Vector2(cameraTexture.width, cameraTexture.height) * scale;
            preview.rectTransform.localEulerAngles = new Vector3(0, 0, -angle);
            preview.uvRect = cameraTexture.videoVerticallyMirrored
                ? new Rect(0, 1, 1, -1) : new Rect(0, 0, 1, 1);
        }

        private void OnDestroy()
        {
            StopAllCoroutines();
            if (cameraTexture != null) { cameraTexture.Stop(); Destroy(cameraTexture); }
            if (canvasObject != null) Destroy(canvasObject);
        }
    }
}
