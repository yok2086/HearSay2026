using UnityEngine;
using UnityEngine.SceneManagement;

namespace HearSay
{
    public sealed class HearSayVrHome : MonoBehaviour
    {
        private int target = -1;
        private float since;
        private Rect[] buttons;
        private void Start()
        {
            buttons = new[] { new Rect(85, 350, 365, 54), new Rect(530, 350, 385, 54) };
#if UNITY_ANDROID && !UNITY_EDITOR
            var loader = ScriptableObject.CreateInstance<Google.XR.Cardboard.XRLoader>();
            loader.Initialize(); loader.Start();
#endif
        }
        private void OnGUI()
        {
            GUI.color = new Color(.035f, .055f, .09f); GUI.DrawTexture(new Rect(0, 0, Screen.width, Screen.height), Texture2D.whiteTexture); GUI.color = Color.white;
            float s = Mathf.Min(Screen.width / 1000f, Screen.height / 650f); var old = GUI.matrix; GUI.matrix = Matrix4x4.TRS(Vector3.zero, Quaternion.identity, Vector3.one * s);
            GUI.Label(new Rect(60, 28, 880, 30), "HEARSAY  /  CONNECTED CONVERSATIONS", HearSayTheme.Label(16, HearSayTheme.Accent));
            GUI.Label(new Rect(60, 70, 880, 65), "A clearer conversation.", HearSayTheme.Label(46, Color.white));
            GUI.Label(new Rect(60, 140, 880, 40), "Choose how you want to connect.", HearSayTheme.Label(23, HearSayTheme.Muted));
            HearSayTheme.Card(new Rect(60, 205, 425, 220), HearSayTheme.Panel); HearSayTheme.Card(new Rect(505, 205, 435, 220), HearSayTheme.Panel);
            GUI.Label(new Rect(85, 222, 365, 45), "Deaf user", HearSayTheme.Label(30, Color.white)); GUI.Label(new Rect(85, 272, 365, 66), "Find the speaker and follow live captions.", HearSayTheme.Label(21, HearSayTheme.Muted));
            GUI.Label(new Rect(530, 222, 380, 45), "Conversation partner", HearSayTheme.Label(28, Color.white)); GUI.Label(new Rect(530, 272, 380, 66), "See sign-language translations in your surroundings.", HearSayTheme.Label(21, HearSayTheme.Muted));
            if (HearSayTheme.Action(buttons[0], "Open live captions"))
            {
                PlayerPrefs.SetInt("HearSay.VrRole", 1); PlayerPrefs.Save();
                SceneManager.LoadScene("HearSay Demo");
            }
            if (HearSayTheme.Action(buttons[1], "Open ASL translation"))
            {
                PlayerPrefs.SetInt("HearSay.VrRole", 0); PlayerPrefs.Save();
                SceneManager.LoadScene("HearSay Demo");
            }
            HearSayTheme.Card(new Rect(60, 448, 310, 48), HearSayTheme.Panel);
            HearSayTheme.Card(new Rect(390, 448, 550, 48), HearSayTheme.Panel);
            GUI.Label(new Rect(60, 448, 310, 48), "Data monitor / teammate", HearSayTheme.Label(18, Color.white, TextAnchor.MiddleCenter));
            GUI.Label(new Rect(390, 448, 550, 48), "Connection settings   +", HearSayTheme.Label(19, Color.white, TextAnchor.MiddleCenter));
#if UNITY_ANDROID && !UNITY_EDITOR
            var gaze = GUI.matrix.inverse.MultiplyPoint3x4(new Vector3(Screen.width * .5f, Screen.height * .5f)); int next = -1; for (int i = 0; i < buttons.Length; i++) if (buttons[i].Contains(gaze)) next = i;
            if (next != target) { target = next; since = Time.unscaledTime; } GUI.Label(new Rect(gaze.x - 20, gaze.y - 20, 40, 40), "○", HearSayTheme.Label(34, HearSayTheme.Accent, TextAnchor.MiddleCenter));
            if (target >= 0 && Time.unscaledTime - since > 3f) SceneManager.LoadScene("HearSay Demo");
#endif
            GUI.matrix = old;
        }
    }
}
