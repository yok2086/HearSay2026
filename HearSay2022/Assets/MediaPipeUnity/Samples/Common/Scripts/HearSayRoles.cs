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
        // Optional mqtt:// address of the local MQTT demo publisher. Empty = Ultra96 MQTT only.
        private string address;
        private string connection = "Ultra96 MQTT. Choose your role.";
        private string translation = "Waiting for a sign…";
        public static string CurrentTranslation { get; private set; } = "Waiting for a sign…";
        private bool partner;
        private HearSayDataMonitor dataMonitor;
        private bool connectionExpanded;
        private Coroutine polling;
        private HearSayPartnerCamera partnerCamera;
        private HearSayPartnerHands partnerHands;
        private HearSaySshMqttManager partnerMqtt;
        private HearSayGloveCalibration calibration;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void SetLandscape()
        {
            Screen.autorotateToPortrait = false;
            Screen.autorotateToPortraitUpsideDown = false;
            Screen.orientation = ScreenOrientation.LandscapeLeft;
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
            CurrentTranslation = "Waiting for a sign…";
            DeafRole = SceneManager.GetActiveScene().name == "HearSay Demo" &&
                PlayerPrefs.GetInt("HearSay.VrRole", 0) == 1;
            Debug.Log("HearSay role init: scene=" + SceneManager.GetActiveScene().name +
                " savedRole=" + PlayerPrefs.GetInt("HearSay.VrRole", 0) +
                " deafRole=" + DeafRole);
            // Old saved http:// dummy-server addresses are ignored: there is no HTTP server any more.
            address = PlayerPrefs.GetString("HearSay.Server", "");
            if (!HearSayLocalMqtt.IsMqtt(address.Trim())) address = "";
            if (SceneManager.GetActiveScene().name == "HearSay Demo" && HearSayLocalMqtt.Valid(address.Trim()))
                SessionServerUrl = address.Trim();
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
                SessionServerUrl = address.Trim();
                HearSayLocalMqtt.For(SessionServerUrl);
                PlayerPrefs.SetString("HearSay.Server", SessionServerUrl);
                PlayerPrefs.Save();
                connection = "MQTT selected. Start the terminal publisher; type clear to clear inputs.";
                return true;
            }
            // No local publisher: ASL and mic data come straight from the Ultra96 over MQTT.
            HearSayLocalMqtt.Stop();
            SessionServerUrl = null;
            PlayerPrefs.SetString("HearSay.Server", "");
            PlayerPrefs.Save();
            return true;
        }

        private void Start()
        {
            if (SceneManager.GetActiveScene().name == "HearSay Demo" && !DeafRole && !partner)
                EnterPartnerRole();
        }

        private void EnterPartnerRole()
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
            partnerMqtt = gameObject.AddComponent<HearSaySshMqttManager>();
            polling = StartCoroutine(PollSigns());
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
                    (SaveAddress() || HearSayVrSplitScreen.Instance != null)) // VR test: open even without the server
                {
                    DeafRole = true;
                    // HearSayLiveCaption only runs Whisper when the saved role is deaf user (1).
                    PlayerPrefs.SetInt("HearSay.VrRole", 1); PlayerPrefs.Save();
                    SceneManager.LoadScene("HearSay Demo");
                }
                if (HearSayTheme.Action(new Rect(530, 350, 385, 54), "Open ASL translation") &&
                    (SaveAddress() || HearSayVrSplitScreen.Instance != null)) // VR test: open even without the server
                {
                    PlayerPrefs.SetInt("HearSay.VrRole", 0); PlayerPrefs.Save();
                    EnterPartnerRole();
                }
                // The data monitor reads the local MQTT demo publisher, so it needs an mqtt:// address.
                if (HearSayLocalMqtt.IsMqtt(address.Trim()) &&
                    HearSayTheme.Action(new Rect(60, 448, 310, 48), "Data monitor / teammate", 18) && SaveAddress())
                {
                    dataMonitor = gameObject.AddComponent<HearSayDataMonitor>();
                    dataMonitor.Initialize(SessionServerUrl);
                }
                if (HearSayTheme.Action(new Rect(390, 448, 550, 48),
                    connectionExpanded ? "Connection settings   −" : "Connection settings   +", 19))
                    connectionExpanded = !connectionExpanded;
                if (connectionExpanded)
                {
                    if (HearSayTheme.Action(new Rect(60, 503, 165, 30), "Ultra96 only", 16))
                    {
                        address = "";
                        HearSayLocalMqtt.Stop();
                        connection = "Ultra96 MQTT. Choose your role.";
                    }
                    if (HearSayTheme.Action(new Rect(240, 503, 165, 30), "MQTT demo", 16))
                    {
                        address = "mqtt://127.0.0.1:1883/hearsay/demo/status";
                        connection = "Local MQTT demo; no HTTP reset required.";
                    }
                    address = GUI.TextField(new Rect(60, 539, 880, 46), address,
                        new GUIStyle(GUI.skin.textField) { fontSize = 22, padding = new RectOffset(16, 16, 10, 10) });
                    GUI.Label(new Rect(60, 590, 880, 45), connection, HearSayTheme.Label(16, HearSayTheme.Muted));
                }
                else
                    GUI.Label(new Rect(60, 520, 880, 45), connection,
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
                if (partnerMqtt != null)
                    serverText += (serverText.Length > 0 ? "  ·  " : "") + "Ultra96: " + partnerMqtt.ConnectionStatus;
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
                    CurrentTranslation = translation;
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

        // Shows whichever source changed most recently: an Ultra96 RESULT (MQTT) or,
        // when an mqtt:// demo address is set, a word from the local MQTT demo publisher.
        private IEnumerator PollSigns()
        {
            string lastHttpSign = null, lastMqttEvent = null;
            float httpChangedAt = 0f, mqttChangedAt = 0f;
            while (true)
            {
                string httpWord = null;
                if (!HearSayLocalMqtt.IsMqtt(SessionServerUrl)) connection = "";
                else using (var request = HearSayStatusRequest.Get(SessionServerUrl))
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
                CurrentTranslation = translation;
                yield return new WaitForSecondsRealtime(0.25f);
            }
        }
    }
}
