using UnityEngine;

namespace HearSay
{
    // "Calibrate glove" on the ASL translation screen.
    //
    // Guides the glove wearer through two short poses and talks to both gloves
    // over the existing SSH/MQTT connection (HearSaySshMqttManager):
    //   1. Open hands, fingers straight -> each glove re-measures its straight-finger
    //      baselines (firmware: CALIBRATE on cmd/<glove>/set, replies CALIBRATION_ACK).
    //   2. Make a fist, then open again -> checks every finger reads bent / straight
    //      in the gloves' live data, so a loose or mis-set sensor shows up as [X].
    // Drawn inside HearSayRoles' OnGUI (1000 x 650 layout), so it works in VR too.
    public sealed class HearSayGloveCalibration : MonoBehaviour
    {
        private enum Step { Closed, Intro, OpenCountdown, WaitAck, FistCountdown, OpenAgain, Result }

        private static readonly string[] FingerNames = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
        private const float PoseSeconds = 3f;
        private const float AckTimeoutSeconds = 6f;
        private const float RequiredFraction = 0.6f; // finger must read right in 60% of samples

        private HearSaySshMqttManager mqtt;
        private Step step = Step.Closed;
        private float stepStart;
        private float calibrateRequestedAt;
        private readonly int[,] fistBent = new int[2, 5];
        private readonly int[,] openStraight = new int[2, 5];
        private readonly int[] fistSamples = new int[2];
        private readonly int[] openSamples = new int[2];
        private readonly bool[] acked = new bool[2];
        private readonly string[] baselines = new string[2];
        private int lastSampleFrame = -1;

        public bool IsOpen => step != Step.Closed;

        public void Open(HearSaySshMqttManager connection)
        {
            mqtt = connection;
            Go(Step.Intro);
        }

        public void Close() => step = Step.Closed;

        private void Go(Step next)
        {
            step = next;
            stepStart = Time.unscaledTime;
            if (next == Step.FistCountdown || next == Step.OpenCountdown)
                for (int g = 0; g < 2; g++)
                {
                    fistSamples[g] = openSamples[g] = 0;
                    for (int f = 0; f < 5; f++) fistBent[g, f] = openStraight[g, f] = 0;
                }
        }

        private float Elapsed => Time.unscaledTime - stepStart;

        private void Update()
        {
            if (step == Step.Closed || mqtt == null) return;
            switch (step)
            {
                case Step.OpenCountdown:
                    if (Elapsed >= PoseSeconds)
                    {
                        calibrateRequestedAt = Time.time;
                        for (int g = 0; g < 2; g++)
                        {
                            acked[g] = false;
                            baselines[g] = null;
                            mqtt.RequestCalibration(HearSaySshMqttManager.GloveIds[g]);
                        }
                        Go(Step.WaitAck);
                    }
                    break;
                case Step.WaitAck:
                    bool all = true;
                    for (int g = 0; g < 2; g++)
                    {
                        if (!acked[g] && mqtt.TryGetCalibration(HearSaySshMqttManager.GloveIds[g],
                                calibrateRequestedAt, out int[] b))
                        {
                            acked[g] = true;
                            baselines[g] = string.Join(", ", b);
                        }
                        all &= acked[g];
                    }
                    if (all || Elapsed >= AckTimeoutSeconds) Go(Step.FistCountdown);
                    break;
                case Step.FistCountdown:
                    Sample(fistBent, fistSamples, bentWanted: true);
                    if (Elapsed >= PoseSeconds) Go(Step.OpenAgain);
                    break;
                case Step.OpenAgain:
                    Sample(openStraight, openSamples, bentWanted: false);
                    if (Elapsed >= PoseSeconds) Go(Step.Result);
                    break;
            }
        }

        // Counts, over the last 1.5 s of a pose, how often each finger reads as wanted.
        private void Sample(int[,] hits, int[] samples, bool bentWanted)
        {
            if (Elapsed < PoseSeconds - 1.5f || Time.frameCount == lastSampleFrame) return;
            lastSampleFrame = Time.frameCount;
            for (int g = 0; g < 2; g++)
            {
                if (!mqtt.TryGetFlex(HearSaySshMqttManager.GloveIds[g], 0.5f, out int[] flex)) continue;
                samples[g]++;
                for (int f = 0; f < 5; f++)
                    if ((flex[f] == 1) == bentWanted) hits[g, f]++;
            }
        }

        // Called from HearSayRoles.OnGUI inside its 1000 x 650 GUI matrix.
        public void Draw()
        {
            if (step == Step.Closed) return;
            HearSayTheme.Card(new Rect(110, 100, 780, 450), HearSayTheme.Navy);
            HearSayTheme.Card(new Rect(110, 100, 780, 450), HearSayTheme.Panel);
            GUI.Label(new Rect(140, 115, 720, 30), "GLOVE CALIBRATION", HearSayTheme.Label(16, HearSayTheme.Accent));

            switch (step)
            {
                case Step.Intro:
                    Title("Make the glove readings accurate");
                    Body("Takes about 15 seconds. The person wearing the gloves follows the steps:\n" +
                         "1. Open both hands, fingers straight, and keep still.\n" +
                         "2. Make a fist with both hands.\n" +
                         "3. Open your hands again.");
                    if (mqtt == null || !mqtt.IsConnected)
                        GUI.Label(new Rect(140, 400, 720, 40),
                            "Not connected to the Ultra96 yet (" + (mqtt != null ? mqtt.ConnectionStatus : "no connection") +
                            "). Wait for “Connected over SSH”, then start.",
                            HearSayTheme.Label(17, new Color(1f, 0.75f, 0.4f), TextAnchor.MiddleCenter));
                    if (HearSayTheme.Action(new Rect(150, 460, 330, 54), "Start calibration") && mqtt != null && mqtt.IsConnected)
                        Go(Step.OpenCountdown);
                    if (HearSayTheme.Action(new Rect(520, 460, 330, 54), "Cancel")) Close();
                    break;

                case Step.OpenCountdown:
                    Title("Open both hands, fingers straight");
                    Body("Keep your hands still…");
                    Countdown();
                    break;

                case Step.WaitAck:
                    Title("Measuring…");
                    Body("Keep your hands open and still.\n\n" + GloveAckLines());
                    break;

                case Step.FistCountdown:
                    Title("Now make a fist with both hands");
                    Body("Squeeze all five fingers…\n\n" + GloveAckLines());
                    Countdown();
                    break;

                case Step.OpenAgain:
                    Title("Open your hands again");
                    Body("Fingers straight…");
                    Countdown();
                    break;

                case Step.Result:
                    Title(AllGood() ? "Calibration done - all fingers OK" : "Calibration done - check the [X] fingers");
                    GUI.Label(new Rect(140, 210, 720, 230), ResultText(), HearSayTheme.Label(19, Color.white, TextAnchor.UpperLeft));
                    if (HearSayTheme.Action(new Rect(150, 460, 330, 54), "Done")) Close();
                    if (HearSayTheme.Action(new Rect(520, 460, 330, 54), "Redo")) Go(Step.Intro);
                    break;
            }
        }

        private void Title(string text) =>
            GUI.Label(new Rect(140, 150, 720, 50), text, HearSayTheme.Label(30, Color.white));

        private void Body(string text) =>
            GUI.Label(new Rect(140, 210, 720, 190), text, HearSayTheme.Label(21, HearSayTheme.Muted, TextAnchor.UpperLeft));

        private void Countdown()
        {
            int left = Mathf.Max(1, Mathf.CeilToInt(PoseSeconds - Elapsed));
            GUI.Label(new Rect(140, 380, 720, 80), left.ToString(), HearSayTheme.Label(60, HearSayTheme.Accent, TextAnchor.MiddleCenter));
        }

        private string GloveAckLines()
        {
            string text = "";
            for (int g = 0; g < 2; g++)
            {
                string id = HearSaySshMqttManager.GloveIds[g];
                text += acked[g] ? $"[OK] {id} calibrated (straight levels: {baselines[g]})\n"
                    : step == Step.WaitAck ? $"… waiting for {id}\n"
                    : $"[X] {id} did not reply (is it on and connected? old firmware?)\n";
            }
            return text;
        }

        private bool FingerOk(int g, int f) =>
            fistSamples[g] > 0 && openSamples[g] > 0 &&
            fistBent[g, f] >= RequiredFraction * fistSamples[g] &&
            openStraight[g, f] >= RequiredFraction * openSamples[g];

        private bool AllGood()
        {
            for (int g = 0; g < 2; g++)
            {
                if (!acked[g]) return false;
                for (int f = 0; f < 5; f++) if (!FingerOk(g, f)) return false;
            }
            return true;
        }

        private string ResultText()
        {
            string text = "";
            for (int g = 0; g < 2; g++)
            {
                string id = HearSaySshMqttManager.GloveIds[g];
                text += (acked[g] ? "[OK] " : "[X] ") + id + (acked[g] ? " calibrated" : " did not reply to calibrate") + "\n";
                if (fistSamples[g] == 0 || openSamples[g] == 0)
                {
                    text += "    No live finger data received from this glove.\n\n";
                    continue;
                }
                text += "    ";
                for (int f = 0; f < 5; f++)
                    text += (FingerOk(g, f) ? "[OK] " : "[X] ") + FingerNames[f] + "   ";
                text += "\n\n";
            }
            if (!AllGood())
                text += "[X] = that finger did not read bent in the fist and straight when open.\n" +
                        "Check the sensor sits flat on the finger, then press Redo.";
            return text;
        }
    }
}
