using System;
using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.XR;
using UnityEngine.XR.Management;
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
        // The Home/Demo screens use OnGUI. In this mode Cardboard presents the
        // original camera directly instead of replacing it with the Canvas-only
        // compositor (which would make the Home screen black).
        public bool preserveOriginalCamera = true;
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

        private void Awake()
        {
            Active = true;
            SceneManager.sceneLoaded += OnSceneLoaded;
            // Keep the Cardboard session alive when the VR Home loads the
            // existing HearSay Demo scene. Normal scenes never create this
            // adapter, so their phone layout remains unchanged.
            DontDestroyOnLoad(gameObject);
        }

        private IEnumerator Start()
        {
            Active = true;
            previousSleep = Screen.sleepTimeout;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;
            // Let the copied scene create its runtime captions and camera UI first.
            yield return null;
            EnsureSceneOverlay(SceneManager.GetActiveScene().name);
#if UNITY_ANDROID && !UNITY_EDITOR
            if (Camera.main != null && Camera.main.GetComponent<HearSayVrGyroFallback>() == null)
                Camera.main.gameObject.AddComponent<HearSayVrGyroFallback>();
            var xrManager = XRGeneralSettings.Instance != null ? XRGeneralSettings.Instance.Manager : null;
            if (xrManager != null)
            {
                yield return xrManager.InitializeLoader();
                if (xrManager.activeLoader == null)
                {
                    failure = "VR could not start: Cardboard XR loader did not initialize.";
                    yield break;
                }
                xrManager.StartSubsystems();
            }
            else
            {
                // This project has no XRGeneralSettings asset wired into
                // ProjectSettings, so use Cardboard's loader directly.
                loader = ScriptableObject.CreateInstance<Google.XR.Cardboard.XRLoader>();
                if (!loader.Initialize() || !loader.Start())
                {
                    failure = "VR could not start: Cardboard loader failed.";
                    yield break;
                }
            }
#endif
            try
            {
                if (SceneManager.GetActiveScene().name == "Final VR HearSay" && Camera.main != null)
                {
                    Camera.main.clearFlags = CameraClearFlags.SolidColor;
                    Camera.main.backgroundColor = new Color(0.035f, 0.055f, 0.09f, 1f);
#if UNITY_ANDROID && !UNITY_EDITOR
                    if (Camera.main.GetComponent<HearSayVrGyroFallback>() == null)
                        Camera.main.gameObject.AddComponent<HearSayVrGyroFallback>();
#endif
                }
#if UNITY_ANDROID && !UNITY_EDITOR
                // Use Unity's XR manager so Cardboard owns the camera pose and
                // updates head tracking every frame.
                var displays = new List<XRDisplaySubsystem>();
                SubsystemManager.GetSubsystems(displays);
                if (!displays.Exists(display => display.running))
                    throw new InvalidOperationException("No running Cardboard display subsystem.");
#endif
                if (!preserveOriginalCamera)
                    CreateDisplay();
                ready = true;
                Debug.Log(preserveOriginalCamera
                    ? "HearSay VR: Cardboard enabled with the original HearSay camera/UI."
                    : "HearSay VR: mono camera + existing UI sent to both eye views.");
            }
            catch (Exception e)
            {
                failure = "VR could not start: " + e.Message;
                Debug.LogException(e);
                StopLoader();
                // Never leave the app in a half-VR state. The original Home UI
                // must remain visible even when Cardboard is unavailable.
                Active = false;
                ready = false;
            }
            while (ready)
            {
                // Restore the original canvas transforms only AFTER both eyes render.
                yield return new WaitForEndOfFrame();
                RestoreCanvases();
            }
        }

        private void OnSceneLoaded(Scene scene, LoadSceneMode mode)
        {
            EnsureSceneOverlay(scene.name);
        }

        private void EnsureSceneOverlay(string sceneName)
        {
            if (sceneName == "Final VR HearSay" && GetComponent<HearSayVrHomeCanvas>() == null)
                gameObject.AddComponent<HearSayVrHomeCanvas>();
            else if (sceneName == "HearSay Demo" && GetComponent<HearSayVrRoleOverlay>() == null)
                gameObject.AddComponent<HearSayVrRoleOverlay>();
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
            {
                PlayerPrefs.SetInt("HearSay.VRMode", 0);
                PlayerPrefs.Save();
                SceneManager.LoadScene("HearSay Start");
            }
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
            if (!ready || preserveOriginalCamera) return;
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
#if UNITY_ANDROID && !UNITY_EDITOR
            if (failure == null && Active)
            {
                var old = GUI.color;
                GUI.color = new Color(0.25f, 0.85f, 0.9f, 0.9f);
                GUI.Label(new Rect(Screen.width * 0.5f - 24f, Screen.height * 0.5f - 24f,
                    48f, 48f), "○", new GUIStyle(GUI.skin.label)
                    { fontSize = 42, alignment = TextAnchor.MiddleCenter });
                GUI.color = old;
            }
#endif
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
            SceneManager.sceneLoaded -= OnSceneLoaded;
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
