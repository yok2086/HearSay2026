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
        private Coroutine polling;
        private HearSayPartnerCamera partnerCamera;

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
            DeafRole = false;
            address = PlayerPrefs.GetString("HearSay.Server", "http://192.168.1.60:8080/status");
        }

        private bool SaveAddress()
        {
            if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) ||
                (uri.Scheme != "http" && uri.Scheme != "https"))
            {
                connection = "Enter the full http:// address printed in Terminal.";
                return false;
            }
            SessionServerUrl = new Uri(uri, "/status").AbsoluteUri;
            PlayerPrefs.SetString("HearSay.Server", SessionServerUrl);
            PlayerPrefs.Save();
            return true;
        }

        private void OnGUI()
        {
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
                    polling = StartCoroutine(PollSigns());
                }
                GUI.Label(new Rect(60, 447, 880, 30), "CONNECTION  /  Use the same server on both phones",
                    HearSayTheme.Label(17, HearSayTheme.Muted));
                address = GUI.TextField(new Rect(60, 485, 880, 52), address,
                    new GUIStyle(GUI.skin.textField) { fontSize = 23, padding = new RectOffset(16, 16, 12, 12) });
                GUI.Label(new Rect(60, 549, 880, 65), connection, HearSayTheme.Label(18, HearSayTheme.Muted));
            }
            else
            {
                HearSayTheme.Card(new Rect(40, 30, 390, 65), HearSayTheme.Panel);
                GUI.Label(new Rect(60, 38, 350, 48), "HearSay  /  Conversation partner", HearSayTheme.Label(21, Color.white));
                HearSayTheme.Card(new Rect(160, 345, 680, 225), HearSayTheme.Panel);
                GUI.Label(new Rect(185, 357, 630, 32), "SIGN LANGUAGE", HearSayTheme.Label(16, HearSayTheme.Accent));
                GUI.Label(new Rect(185, 395, 630, 115), translation,
                    HearSayTheme.Label(36, Color.white, TextAnchor.MiddleCenter));
                GUI.Label(new Rect(185, 523, 630, 30), connection,
                    HearSayTheme.Label(16, HearSayTheme.Muted, TextAnchor.MiddleCenter));
                if (HearSayTheme.Action(new Rect(350, 585, 300, 48), "Change role / connection", 19))
                {
                    if (polling != null) StopCoroutine(polling);
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
                            connection = "SERVER: CONNECTED";
                            translation = sign.aslDetected && !string.IsNullOrEmpty(sign.aslWord)
                                ? "ASL DETECTED\n" + sign.aslWord.Replace('_', ' ')
                                : "Waiting for a sign…";
                        }
                        catch (Exception) { connection = "SERVER: INVALID DATA"; translation = "Waiting for a sign…"; }
                    }
                    else
                    {
                        connection = "SERVER: " + request.error;
                        translation = "Waiting for connection…";
                    }
                }
                yield return new WaitForSecondsRealtime(0.25f);
            }
        }
    }
}
