using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.XR;
using UnityEngine.UI;
using CardboardApi = Google.XR.Cardboard.Api;

namespace HearSay
{
    // VR-only adapter. The existing detector and UI still operate in their original
    // full-screen coordinate system. Their composed image is presented to both eyes.
    [DefaultExecutionOrder(32000)]
    public sealed class HearSayVrDisplay : MonoBehaviour
    {
        public static bool Active { get; private set; }
        [SerializeField] private Shader displayShader;
        private Google.XR.Cardboard.XRLoader loader;
        private Camera captureCamera;
        private GameObject viewer;
        private RenderTexture composition;
        private Material material;
        private readonly List<CanvasState> saved = new List<CanvasState>();
        private readonly List<Camera> disabledCameras = new List<Camera>();
        private Canvas[] canvases = Array.Empty<Canvas>();
        private bool ready;
        private string failure;
        private int previousSleep;
        private float nextCanvasScan;
        private const int DisplayLayer = 31;

        private struct CanvasState
        {
            public Canvas canvas;
            public RenderMode mode;
            public Camera camera;
            public float distance;
            public bool enabled;
        }

        private void Awake() { Active = true; }

        private IEnumerator Start()
        {
            Active = true;
            previousSleep = Screen.sleepTimeout;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;
            // Let the copied scene create its runtime captions and camera UI first.
            yield return null;
            try
            {
#if UNITY_ANDROID && !UNITY_EDITOR
                // A private loader avoids enabling XR on startup in the normal scenes.
                loader = ScriptableObject.CreateInstance<Google.XR.Cardboard.XRLoader>();
                if (!loader.Initialize() || !loader.Start())
                    throw new InvalidOperationException("Cardboard could not start.");
                var displays = new List<XRDisplaySubsystem>();
                SubsystemManager.GetSubsystems(displays);
                if (!displays.Exists(display => display.running))
                    throw new InvalidOperationException("No running Cardboard display subsystem.");
#endif
                CreateDisplay();
                ready = true;
                Debug.Log("HearSay VR: mono camera + existing UI sent to both eye views.");
            }
            catch (Exception e)
            {
                failure = "VR could not start: " + e.Message;
                Debug.LogException(e);
                StopLoader();
            }
            while (ready)
            {
                // Restore the original canvas transforms only AFTER both eyes render.
                yield return new WaitForEndOfFrame();
                RestoreCanvases();
            }
        }

        private void CreateDisplay()
        {
            foreach (var camera in FindObjectsByType<Camera>(FindObjectsSortMode.None))
            {
                if (!camera.enabled) continue;
                disabledCameras.Add(camera);
                camera.enabled = false;
            }
            composition = new RenderTexture(Screen.width, Screen.height, 24, RenderTextureFormat.ARGB32);
            composition.name = "HearSay VR Composition";
            composition.Create();
            captureCamera = new GameObject("VR UI capture camera").AddComponent<Camera>();
            captureCamera.transform.SetParent(transform, false);
            captureCamera.enabled = false;
            captureCamera.stereoTargetEye = StereoTargetEyeMask.None;
            captureCamera.targetTexture = composition;
            captureCamera.clearFlags = CameraClearFlags.SolidColor;
            captureCamera.backgroundColor = Color.black;
            captureCamera.cullingMask = ~(1 << DisplayLayer); // Runtime UI also uses Default layer.
            captureCamera.nearClipPlane = 0.01f;
            captureCamera.farClipPlane = 100f;
            captureCamera.orthographic = true;
            captureCamera.orthographicSize = Screen.height * 0.5f;
            captureCamera.transform.position = new Vector3(0f, 0f, -50f);

            viewer = new GameObject("Cardboard HearSay viewer");
            viewer.transform.SetParent(transform, false);
            var eye = viewer.AddComponent<Camera>();
            eye.clearFlags = CameraClearFlags.SolidColor;
            eye.backgroundColor = Color.black;
            eye.cullingMask = 1 << DisplayLayer;
            eye.nearClipPlane = 0.01f;
            eye.farClipPlane = 10f;
            eye.fieldOfView = 60f;
            eye.stereoTargetEye = StereoTargetEyeMask.Both;
            var panel = GameObject.CreatePrimitive(PrimitiveType.Quad);
            panel.name = "Mono passthrough and overlays";
            panel.layer = DisplayLayer;
            Destroy(panel.GetComponent<Collider>());
            panel.transform.SetParent(viewer.transform, false);
            panel.transform.localPosition = new Vector3(0f, 0f, 2f);
            float aspect = (float)composition.width / composition.height;
            panel.transform.localScale = new Vector3(2.4f, 2.4f / aspect, 1f);
            if (displayShader == null) throw new InvalidOperationException("VR display shader is missing.");
            material = new Material(displayShader) { mainTexture = composition };
            panel.GetComponent<MeshRenderer>().sharedMaterial = material;
#if UNITY_EDITOR
            // Layout preview only: real lens correction runs on the Android device.
            eye.stereoTargetEye = StereoTargetEyeMask.None;
            eye.rect = new Rect(0f, 0f, 0.5f, 1f);
            var right = new GameObject("Right-eye layout preview").AddComponent<Camera>();
            right.transform.SetParent(viewer.transform, false);
            right.CopyFrom(eye);
            right.rect = new Rect(0.5f, 0f, 0.5f, 1f);
#endif
        }

        private void Update()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!ready) return;
            CardboardApi.UpdateScreenParams();
            if (CardboardApi.IsCloseButtonPressed)
                SceneManager.LoadScene("HearSay Start");
            // Do not launch a QR camera while MediaPipe owns the rear camera.
            if (CardboardApi.IsGearButtonPressed)
            {
                Debug.Log("Use the VR setup screen to scan your viewer QR code.");
                SceneManager.LoadScene("HearSay VR Setup");
            }
#endif
        }

        private void LateUpdate()
        {
            if (!ready) return;
            RestoreCanvases();
            if (Time.unscaledTime >= nextCanvasScan)
            {
                canvases = FindObjectsByType<Canvas>(FindObjectsSortMode.None);
                nextCanvasScan = Time.unscaledTime + 0.5f;
            }
            // Change display routing for rendering only. Tracking/caption code sees
            // its original overlay canvas during Update and LateUpdate next frame.
            try
            {
                foreach (var canvas in canvases)
                {
                    if (canvas == null || !canvas.isRootCanvas || !canvas.isActiveAndEnabled) continue;
                    saved.Add(new CanvasState { canvas = canvas, mode = canvas.renderMode,
                        camera = canvas.worldCamera, distance = canvas.planeDistance, enabled = canvas.enabled });
                    canvas.renderMode = RenderMode.ScreenSpaceCamera;
                    canvas.worldCamera = captureCamera;
                    canvas.planeDistance = 1f;
                }
                Canvas.ForceUpdateCanvases();
                captureCamera.Render();
                // Prevent a second, flat overlay being drawn across the two eye views.
                foreach (var state in saved) if (state.canvas != null) state.canvas.enabled = false;
            }
            catch (Exception e)
            {
                RestoreCanvases();
                ready = false;
                failure = "VR composition failed: " + e.Message;
                Debug.LogException(e);
            }
        }

        private void RestoreCanvases()
        {
            foreach (var state in saved)
            {
                if (state.canvas == null) continue;
                state.canvas.renderMode = state.mode;
                state.canvas.worldCamera = state.camera;
                state.canvas.planeDistance = state.distance;
                state.canvas.enabled = state.enabled;
            }
            saved.Clear();
            Canvas.ForceUpdateCanvases();
        }

        private void OnGUI()
        {
            if (failure == null) return;
            GUI.Box(new Rect(20, 20, Screen.width - 40, 150), failure);
            if (GUI.Button(new Rect(40, 95, 250, 55), "Return to normal HearSay"))
                SceneManager.LoadScene("HearSay Start");
        }

        private void StopLoader()
        {
            if (loader == null) return;
            loader.Stop();
            loader.Deinitialize();
            Destroy(loader);
            loader = null;
        }

        private void OnDestroy()
        {
            ready = false;
            RestoreCanvases();
            StopLoader();
            foreach (var camera in disabledCameras) if (camera != null) camera.enabled = true;
            if (composition != null) { composition.Release(); Destroy(composition); }
            if (material != null) Destroy(material);
            Screen.sleepTimeout = previousSleep;
            Active = false;
        }
    }
}
