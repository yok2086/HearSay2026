using System;
using System.Collections;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.SceneManagement;
using CG4002.Comms;

namespace HearSay
{
    // Camera capture starts only when the partner role is chosen.
    public class HearSayRoles : MonoBehaviour
    {
        public static bool DeafRole { get; private set; }
        public static string SessionServerUrl { get; private set; }
        private string address;
        private string connection = "Waiting to connect";
        private string translation = "Waiting for a sign…";
        private bool partner;
        private HearSayDataMonitor dataMonitor;
        private bool connectionExpanded;
        private Coroutine polling;
        private HearSayPartnerCamera partnerCamera;
        private HearSayPartnerHands partnerHands;
        private HearSaySshMqttManager partnerMqtt;
        private HearSayGloveCalibration calibration;
        private static string resetServerUrl;
        private bool resetInProgress;
        private const float TunnelTimeoutSeconds = 60f;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void SetLandscape()
        {
            resetServerUrl = null;
            Screen.autorotateToPortrait = false;
            Screen.autorotateToPortraitUpsideDown = false;
            Screen.orientation = ScreenOrientation.LandscapeLeft;
            Application.quitting += ServerTunnel.Stop;
        }

        // A loopback address means the server is reached through the phone's SSH
        // tunnel (stujump -> ultra96 -> Mac, see run_hearsay_pipeline.sh).
        public static bool EnsureTunnel(string url)
        {
            if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) || !uri.IsLoopback) return false;
            ServerTunnel.EnsureStarted(uri.Port);
            return true;
        }

        [Serializable]
        private class SignStatus
        {
            public bool aslDetected;
            public string aslWord;
            public float aslConfidence;
        }

        private void Awake()
        {
            DeafRole = SceneManager.GetActiveScene().name == "HearSay Demo" &&
                PlayerPrefs.GetInt("HearSay.VrRole", 0) == 1;
            const string currentDefault = "http://172.25.96.216:8080/status";
            address = PlayerPrefs.GetString("HearSay.Server", currentDefault);
            if (SceneManager.GetActiveScene().name == "HearSay Demo" &&
                Uri.TryCreate(address.Trim(), UriKind.Absolute, out var savedUri) &&
                (savedUri.Scheme == "http" || savedUri.Scheme == "https"))
                SessionServerUrl = new Uri(savedUri, "/status").AbsoluteUri;
            // One-time migration of the previous network's default; preserve custom addresses.
            if (!PlayerPrefs.HasKey("HearSay.ServerAddressMigrationV2"))
            {
                if (address.Trim().TrimEnd('/') == "http://192.168.1.60:8080/status")
                    address = currentDefault;
                PlayerPrefs.SetString("HearSay.Server", address);
                PlayerPrefs.SetInt("HearSay.ServerAddressMigrationV2", 1);
                PlayerPrefs.Save();
            }
        }

        private bool SaveAddress()
        {
            if (HearSayLocalMqtt.IsMqtt(address.Trim()))
            {
                if (!HearSayLocalMqtt.Valid(address.Trim()))
                {
                    connection = "Use mqtt://HOST:1883/hearsay/demo/status";
                    return false;
                }
                if (resetInProgress) { StopAllCoroutines(); resetInProgress = false; }
                SessionServerUrl = address.Trim();
                HearSayLocalMqtt.For(SessionServerUrl);
                PlayerPrefs.SetString("HearSay.Server", SessionServerUrl);
                PlayerPrefs.Save();
                connection = "MQTT selected. Start the terminal publisher; type clear to clear inputs.";
                return true;
            }
            HearSayLocalMqtt.Stop();
            if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) ||
                (uri.Scheme != "http" && uri.Scheme != "https"))
            {
                connection = "Enter the full http:// address printed in Terminal.";
                connectionExpanded = true;
                return false;
            }
            SessionServerUrl = new Uri(uri, "/status").AbsoluteUri;
            // Persist BEFORE the request: network/reset errors must not restore the old IP.
            address = SessionServerUrl;
            PlayerPrefs.SetString("HearSay.Server", SessionServerUrl);
            PlayerPrefs.Save();
            if (resetInProgress) return false;
            if (resetServerUrl != SessionServerUrl)
            {
                StartCoroutine(ResetServer(SessionServerUrl));
                return false;
            }
            return true;
        }

        private void Start()
        {
            if (resetServerUrl != null) return;
            if (Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) &&
                (uri.Scheme == "http" || uri.Scheme == "https"))
                StartCoroutine(ResetServer(new Uri(uri, "/status").AbsoluteUri));
        }

        private IEnumerator ResetServer(string statusUrl)
        {
            resetInProgress = true;
            bool tunnelled = EnsureTunnel(statusUrl);
            if (tunnelled)
            {
                float deadline = Time.realtimeSinceStartup + TunnelTimeoutSeconds;
                while (!ServerTunnel.Ready && Time.realtimeSinceStartup < deadline)
                {
                    connection = "Opening SSH tunnel to Ultra96… " + ServerTunnel.Status;
                    yield return null;
                }
                if (!ServerTunnel.Ready)
                {
                    connection = "SSH tunnel not ready: " + ServerTunnel.Status + " Select your role to retry.";
                    connectionExpanded = true;
                    resetInProgress = false;
                    yield break;
                }
            }
            connection = "Clearing previous sound and ASL commands…";
            using (var request = new UnityWebRequest(new Uri(new Uri(statusUrl), "/reset").AbsoluteUri, "POST"))
            {
                request.downloadHandler = new DownloadHandlerBuffer();
                request.timeout = 3;
                UnityWebRequestAsyncOperation operation = null;
                try { operation = request.SendWebRequest(); }
                catch (Exception e) { connection = "Could not clear server: " + e.Message; }
                if (operation != null) yield return operation;
                if (operation != null && request.result == UnityWebRequest.Result.Success)
                {
                    resetServerUrl = statusUrl;
                    connection = "Sound and ASL cleared. Choose your role.";
                }
                else
                {
                    connection = tunnelled
                        ? "Reset failed through the SSH tunnel. Check HearSayDummyServer.py and run_hearsay_pipeline.sh (8080 reverse forward) are running, then select your role to retry."
                        : "Reset failed. Check the server address and restart the updated Python server, then select your role to retry.";
                    connectionExpanded = true;
                }
            }
            resetInProgress = false;
        }

        private void OnGUI()
        {
            if (dataMonitor != null) return;
            var oldMatrix = GUI.matrix;
            if (!partner)
            {
                var oldColor = GUI.color;
                GUI.color = HearSayTheme.Navy;
                GUI.DrawTexture(new Rect(0, 0, Screen.width, Screen.height), Texture2D.whiteTexture);
                GUI.color = oldColor;
            }
            var safe = Screen.safeArea;
            float scale = Mathf.Min(safe.width / 1000f, safe.height / 650f);
            GUI.matrix = Matrix4x4.TRS(new Vector3(safe.x + (safe.width - 1000 * scale) / 2,
                Screen.height - safe.yMax + (safe.height - 650 * scale) / 2, 0),
                Quaternion.identity, Vector3.one * scale);
            if (!partner)
            {
                GUI.Label(new Rect(60, 28, 880, 30), "HEARSAY  /  CONNECTED CONVERSATIONS",
                    HearSayTheme.Label(16, HearSayTheme.Accent));
                GUI.Label(new Rect(60, 70, 880, 65), "A clearer conversation.",
                    HearSayTheme.Label(46, Color.white));
                GUI.Label(new Rect(60, 140, 880, 40), "Choose how you want to connect.",
                    HearSayTheme.Label(23, HearSayTheme.Muted));

                HearSayTheme.Card(new Rect(60, 205, 425, 220), HearSayTheme.Panel);
                HearSayTheme.Card(new Rect(505, 205, 435, 220), HearSayTheme.Panel);
                GUI.Label(new Rect(85, 222, 365, 45), "Deaf user", HearSayTheme.Label(30, Color.white));
                GUI.Label(new Rect(85, 272, 365, 66), "Find the speaker and follow live captions.", HearSayTheme.Label(21, HearSayTheme.Muted));
                GUI.Label(new Rect(530, 222, 380, 45), "Conversation partner", HearSayTheme.Label(28, Color.white));
                GUI.Label(new Rect(530, 272, 380, 66), "See sign-language translations in your surroundings.", HearSayTheme.Label(21, HearSayTheme.Muted));
                if (HearSayTheme.Action(new Rect(85, 350, 365, 54), "Open live captions") &&
                    (SaveAddress() || HearSayVrSplitScreen.Active)) // VR test: open even without the server
                {
                    DeafRole = true;
                    SceneManager.LoadScene("HearSay Demo");
                }
                if (HearSayTheme.Action(new Rect(530, 350, 385, 54), "Open ASL translation") &&
                    (SaveAddress() || HearSayVrSplitScreen.Active)) // VR test: open even without the server
                {
                    partner = true;
                    partnerCamera = gameObject.AddComponent<HearSayPartnerCamera>();
                    var handPrefab = Resources.Load<HearSayPartnerHands>("HearSayPartnerHands");
                    if (handPrefab != null)
                    {
                        partnerHands = Instantiate(handPrefab, transform);
                        partnerHands.Initialize(partnerCamera);
                    }
                    else Debug.LogError("Partner hand tracking prefab is missing.");
                    // Ultra96 RESULT messages arrive over SSH + MQTT, alongside terminal commands over HTTP.
                    partnerMqtt = gameObject.AddComponent<HearSaySshMqttManager>();
                    polling = StartCoroutine(PollSigns());
                }
                if (HearSayTheme.Action(new Rect(60, 448, 310, 48), "Data monitor / teammate", 18) && SaveAddress())
                {
                    dataMonitor = gameObject.AddComponent<HearSayDataMonitor>();
                    dataMonitor.Initialize(SessionServerUrl);
                }
                if (HearSayTheme.Action(new Rect(390, 448, 550, 48),
                    connectionExpanded ? "Connection settings   −" : "Connection settings   +", 19))
                    connectionExpanded = !connectionExpanded;
                if (connectionExpanded)
                {
                    if (HearSayTheme.Action(new Rect(60, 503, 165, 30), "HTTP", 16))
                    {
                        address = "http://127.0.0.1:8080/status";
                        HearSayLocalMqtt.Stop();
                    }
                    if (HearSayTheme.Action(new Rect(240, 503, 165, 30), "MQTT demo", 16))
                    {
                        if (resetInProgress) { StopAllCoroutines(); resetInProgress = false; }
                        address = "mqtt://127.0.0.1:1883/hearsay/demo/status";
                        connection = "Local MQTT demo; no HTTP reset required.";
                    }
                    address = GUI.TextField(new Rect(60, 539, 880, 46), address,
                        new GUIStyle(GUI.skin.textField) { fontSize = 22, padding = new RectOffset(16, 16, 10, 10) });
                    GUI.Label(new Rect(60, 590, 880, 45), connection, HearSayTheme.Label(16, HearSayTheme.Muted));
                }
                else
                    GUI.Label(new Rect(60, 520, 880, 45), resetInProgress ? "Clearing previous commands…" :
                        (resetServerUrl != null ? "Ready. Waiting for new terminal commands." : connection),
                        HearSayTheme.Label(18, HearSayTheme.Muted, TextAnchor.MiddleCenter));
            }
            else if (calibration != null && calibration.IsOpen)
            {
                // "Calibrate glove" steps take over the ASL translation screen until closed.
                calibration.Draw();
            }
            else
            {
                HearSayTheme.Card(new Rect(40, 30, 390, 65), HearSayTheme.Panel);
                GUI.Label(new Rect(60, 38, 350, 48), "HearSay  /  Conversation partner", HearSayTheme.Label(21, Color.white));
                HearSayTheme.Card(new Rect(160, 375, 680, 195), HearSayTheme.Panel);
                GUI.Label(new Rect(185, 387, 630, 32), "ASL  /  LIVE TRANSLATION", HearSayTheme.Label(16, HearSayTheme.Accent));
                GUI.Label(new Rect(185, 427, 630, 80), translation.Replace("ASL DETECTED\n", ""),
                    HearSayTheme.Label(36, Color.white, TextAnchor.MiddleCenter));
                string serverText = connection == "SERVER: CONNECTED" ? "Connected" : connection;
                if (partnerMqtt != null) serverText += "  ·  Ultra96: " + partnerMqtt.ConnectionStatus;
                GUI.Label(new Rect(185, 528, 630, 26), serverText,
                    HearSayTheme.Label(16, HearSayTheme.Muted, TextAnchor.MiddleCenter));
                if (HearSayTheme.Action(new Rect(450, 38, 230, 50), "Calibrate glove", 19))
                {
                    if (calibration == null) calibration = gameObject.AddComponent<HearSayGloveCalibration>();
                    calibration.Open(partnerMqtt);
                }
                if (HearSayTheme.Action(new Rect(350, 585, 300, 48), "Change role / connection", 19))
                {
                    if (calibration != null) Destroy(calibration);
                    if (polling != null) StopCoroutine(polling);
                    if (partnerHands != null) Destroy(partnerHands.gameObject);
                    if (partnerCamera != null) Destroy(partnerCamera);
                    if (partnerMqtt != null) Destroy(partnerMqtt);
                    partner = false;
                    translation = "Waiting for a sign…";
                }
                if (partnerCamera != null && !string.IsNullOrEmpty(partnerCamera.Status))
                    GUI.Label(new Rect(60, 105, 880, 50), partnerCamera.Status, HearSayTheme.Label(20, Color.white));
            }
            GUI.matrix = oldMatrix;
            if (partner && HomeButton()) ReturnToSelector();
        }

        // Shared screen-space navigation; inset below the microphone indicator.
        public static bool HomeButton()
        {
            float scale = Mathf.Clamp(Screen.height / 650f, 0.75f, 2.5f);
            var safe = Screen.safeArea;
            var rect = new Rect(safe.xMax - 145f * scale,
                Screen.height - safe.yMax + 80f * scale, 130f * scale, 56f * scale);
            return HearSayTheme.Action(rect, "Home", Mathf.RoundToInt(22f * scale));
        }

        public static void ReturnToSelector()
        {
            DeafRole = false;
            // Stay in VR when the session started from the VR start scene.
            SceneManager.LoadScene(HearSayVrSplitScreen.Instance != null
                ? HearSayVrSplitScreen.VrStartScene : "HearSay Start");
        }

        // Shows whichever source changed most recently: a terminal command from the
        // dummy server (HTTP) or an Ultra96 RESULT (MQTT).
        private IEnumerator PollSigns()
        {
            string lastHttpSign = null, lastMqttEvent = null;
            float httpChangedAt = 0f, mqttChangedAt = 0f;
            while (true)
            {
                string httpWord = null;
                using (var request = UnityWebRequest.Get(SessionServerUrl))
                {
                    request.timeout = 2;
                    yield return request.SendWebRequest();
                    if (request.result == UnityWebRequest.Result.Success)
                    {
                        try
                        {
                            var sign = JsonUtility.FromJson<SignStatus>(request.downloadHandler.text);
                            if (sign == null) throw new FormatException("Empty server data");
                            connection = "SERVER: CONNECTED";
                            if (sign.aslDetected && !string.IsNullOrEmpty(sign.aslWord)) httpWord = sign.aslWord;
                        }
                        catch (Exception) { connection = "SERVER: INVALID DATA"; }
                    }
                    else connection = "SERVER: " + request.error;
                }

                if (httpWord != lastHttpSign) { lastHttpSign = httpWord; httpChangedAt = Time.realtimeSinceStartup; }
                string mqttWord = null;
                if (partnerMqtt != null && partnerMqtt.aslDetected)
                {
                    mqttWord = partnerMqtt.aslWord;
                    if (partnerMqtt.lastResultEventId != lastMqttEvent)
                    {
                        lastMqttEvent = partnerMqtt.lastResultEventId;
                        mqttChangedAt = Time.realtimeSinceStartup;
                    }
                }

                string word = mqttWord != null && (httpWord == null || mqttChangedAt >= httpChangedAt)
                    ? mqttWord : httpWord;
                if (partnerHands != null) partnerHands.Word = word?.Replace('_', ' ');
                if (word != null) translation = "ASL DETECTED\n" + word.Replace('_', ' ');
                else translation = connection == "SERVER: CONNECTED" ||
                    (partnerMqtt != null && partnerMqtt.ConnectionStatus == "Connected over SSH")
                    ? "Waiting for a sign…" : "Waiting for connection…";
                yield return new WaitForSecondsRealtime(0.25f);
            }
        }
    }
}
