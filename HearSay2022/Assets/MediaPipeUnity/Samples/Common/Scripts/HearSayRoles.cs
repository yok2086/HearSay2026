using System;
using System.Collections;
using UnityEngine;
using UnityEngine.Networking;
using UnityEngine.SceneManagement;

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
        private static string resetServerUrl;
        private bool resetInProgress;

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void SetLandscape()
        {
            resetServerUrl = null;
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
            DeafRole = false;
            address = PlayerPrefs.GetString("HearSay.Server", "http://192.168.1.60:8080/status");
        }

        private bool SaveAddress()
        {
            if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) ||
                (uri.Scheme != "http" && uri.Scheme != "https"))
            {
                connection = "Enter the full http:// address printed in Terminal.";
                connectionExpanded = true;
                return false;
            }
            SessionServerUrl = new Uri(uri, "/status").AbsoluteUri;
            if (resetInProgress) return false;
            if (resetServerUrl != SessionServerUrl)
            {
                StartCoroutine(ResetServer(SessionServerUrl));
                return false;
            }
            PlayerPrefs.SetString("HearSay.Server", SessionServerUrl);
            PlayerPrefs.Save();
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
                    connection = "Reset failed. Check the server address and restart the updated Python server, then select your role to retry.";
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
                if (HearSayTheme.Action(new Rect(85, 350, 365, 54), "Open live captions") && SaveAddress())
                {
                    DeafRole = true;
                    SceneManager.LoadScene("HearSay Demo");
                }
                if (HearSayTheme.Action(new Rect(530, 350, 385, 54), "Open ASL translation") && SaveAddress())
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
                    GUI.Label(new Rect(60, 505, 880, 26), "SERVER ADDRESS  /  Same on both phones", HearSayTheme.Label(15, HearSayTheme.Muted));
                    address = GUI.TextField(new Rect(60, 539, 880, 46), address,
                        new GUIStyle(GUI.skin.textField) { fontSize = 22, padding = new RectOffset(16, 16, 10, 10) });
                    GUI.Label(new Rect(60, 590, 880, 45), connection, HearSayTheme.Label(16, HearSayTheme.Muted));
                }
                else
                    GUI.Label(new Rect(60, 520, 880, 45), resetInProgress ? "Clearing previous commands…" :
                        (resetServerUrl != null ? "Ready. Waiting for new terminal commands." : connection),
                        HearSayTheme.Label(18, HearSayTheme.Muted, TextAnchor.MiddleCenter));
            }
            else
            {
                HearSayTheme.Card(new Rect(40, 30, 390, 65), HearSayTheme.Panel);
                GUI.Label(new Rect(60, 38, 350, 48), "HearSay  /  Conversation partner", HearSayTheme.Label(21, Color.white));
                HearSayTheme.Card(new Rect(160, 375, 680, 195), HearSayTheme.Panel);
                GUI.Label(new Rect(185, 387, 630, 32), "ASL  /  LIVE TRANSLATION", HearSayTheme.Label(16, HearSayTheme.Accent));
                GUI.Label(new Rect(185, 427, 630, 80), translation.Replace("ASL DETECTED\n", ""),
                    HearSayTheme.Label(36, Color.white, TextAnchor.MiddleCenter));
                GUI.Label(new Rect(185, 528, 630, 26), connection == "SERVER: CONNECTED" ? "Connected" : connection,
                    HearSayTheme.Label(16, HearSayTheme.Muted, TextAnchor.MiddleCenter));
                if (HearSayTheme.Action(new Rect(350, 585, 300, 48), "Change role / connection", 19))
                {
                    if (polling != null) StopCoroutine(polling);
                    if (partnerHands != null) Destroy(partnerHands.gameObject);
                    if (partnerCamera != null) Destroy(partnerCamera);
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
            SceneManager.LoadScene("HearSay Start");
        }

        private IEnumerator PollSigns()
        {
            while (true)
            {
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
                            if (partnerHands != null)
                                partnerHands.Word = sign.aslDetected ? (sign.aslWord ?? "").Replace('_', ' ') : null;
                            connection = "SERVER: CONNECTED";
                            translation = sign.aslDetected && !string.IsNullOrEmpty(sign.aslWord)
                                ? "ASL DETECTED\n" + sign.aslWord.Replace('_', ' ')
                                : "Waiting for a sign…";
                        }
                        catch (Exception)
                        {
                            connection = "SERVER: INVALID DATA";
                            translation = "Waiting for a sign…";
                            if (partnerHands != null) partnerHands.Word = null;
                        }
                    }
                    else
                    {
                        connection = "SERVER: " + request.error;
                        translation = "Waiting for connection…";
                        if (partnerHands != null) partnerHands.Word = null;
                    }
                }
                yield return new WaitForSecondsRealtime(0.25f);
            }
        }
    }
}
