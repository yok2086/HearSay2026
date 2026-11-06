using UnityEngine;

namespace HearSay
{
    // "Calibrate glove" on the ASL translation screen.
    //
    // The glove firmware calibrates itself as soon as it connects to the broker:
    //   one short buzz  = start
    //   STEP 1          = straighten all fingers and hold still (straight readings)
    //   one long buzz   = straight readings saved
    //   two short buzzes= STEP 2: make a comfortable fist and hold (fist readings)
    //   one long buzz   = fist readings saved
    // If any finger reads lower in the fist than when straight, it restarts.
    // Progress arrives as CALIBRATION_STATUS (sys/<glove>/status) and the result as
    // the retained CALIBRATION message (sys/<glove>/calibration). This screen shows
    // that live for both gloves, so the wearer can follow along (also in VR).
    public sealed class HearSayGloveCalibration : MonoBehaviour
    {
        /// Kept for older callers; the screen no longer pops up by itself.
        public static bool OfferedThisSession { get; set; }

        private HearSaySshMqttManager mqtt;
        private bool open;

        public bool IsOpen => open;

        public void Open(HearSaySshMqttManager connection, bool automatic = false)
        {
            mqtt = connection;
            open = true;
            OfferedThisSession = true;
        }

        public void Close() => open = false;

        private static string Glove(int g) => HearSaySshMqttManager.GloveIds[g];
        private static string Name(int g) => g == 0 ? "Left glove" : "Right glove";

        // Called from HearSayRoles.OnGUI inside its 1000 x 650 GUI matrix.
        public void Draw()
        {
            if (!open) return;
            HearSayTheme.Card(new Rect(110, 90, 780, 470), HearSayTheme.Navy);
            HearSayTheme.Card(new Rect(110, 90, 780, 470), HearSayTheme.Panel);
            GUI.Label(new Rect(140, 105, 720, 30), "GLOVE CALIBRATION", HearSayTheme.Label(16, HearSayTheme.Accent));
            GUI.Label(new Rect(140, 140, 720, 50), "The gloves calibrate when they are switched on",
                HearSayTheme.Label(28, Color.white));
            GUI.Label(new Rect(140, 192, 720, 120),
                "1. One short buzz: straighten all fingers and hold still.\n" +
                "2. Two short buzzes: make a comfortable fist and hold.\n" +
                "A long buzz after each step means it was saved. To calibrate again,\n" +
                "switch the glove off and on.",
                HearSayTheme.Label(19, HearSayTheme.Muted, TextAnchor.UpperLeft));

            bool connected = mqtt != null && mqtt.IsConnected;
            for (int g = 0; g < 2; g++)
            {
                string line;
                Color color = Color.white;
                if (!connected)
                {
                    line = "waiting for the Ultra96 connection…";
                    color = HearSayTheme.Muted;
                }
                else if (!mqtt.TryGetGloveCalibration(Glove(g), out var state))
                {
                    line = "no calibration received yet (switch the glove on)";
                    color = HearSayTheme.Muted;
                }
                else if (state.calibrated && (state.stage == "SUCCESS" || state.stage == ""))
                {
                    line = "[OK] calibrated";
                    color = new Color(0.45f, 0.9f, 0.55f);
                }
                else if (state.stage == "FAILED")
                {
                    line = "[X] " + (string.IsNullOrEmpty(state.message) ? "failed, restarting" : state.message);
                    color = new Color(1f, 0.6f, 0.45f);
                }
                else
                {
                    line = "calibrating: " + (string.IsNullOrEmpty(state.message) ? state.stage : state.message);
                    color = HearSayTheme.Accent;
                }
                GUI.Label(new Rect(140, 330 + g * 48, 720, 40), Name(g) + ": " + line,
                    HearSayTheme.Label(21, color, TextAnchor.MiddleLeft));
            }

            if (HearSayTheme.Action(new Rect(335, 470, 330, 58), "Done")) Close();
        }
    }
}
