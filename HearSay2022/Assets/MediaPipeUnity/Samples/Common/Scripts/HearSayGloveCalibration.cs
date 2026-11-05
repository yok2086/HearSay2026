using UnityEngine;

namespace HearSay
{
    // Guided glove calibration on the ASL translation screen.
    //
    // Pops up by itself the first time the Ultra96 connects in a session (and from
    // the "Calibrate glove" button any time after). Follows the team's steps (the "return to straight" check is left out):
    //   1. Ready            "Put on both gloves and relax your hands."   1 short buzz
    //   2. Straight hands   glove saves straight readings                1 short buzz
    //   3. Make fists       glove saves bent readings                    2 short buzzes
    //   5. Complete         glove starts using the new values            1 long buzz
    //   6. Failed           shows which sensors failed, Try again        3 short buzzes
    // Each step is a CALIBRATE command on cmd/<glove>/set; the glove buzzes, waits
    // for the wearer, measures, and answers CALIBRATION_ACK with per-finger results.
    // Drawn inside HearSayRoles' OnGUI (1000 x 650 layout), so it works in VR too.
    public sealed class HearSayGloveCalibration : MonoBehaviour
    {
        private enum Step { Closed, Intro, Ready, Straight, Fist, Saving, Complete, Failed }

        private static readonly string[] FingerNames = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
        private const int SettleMs = 2500;          // time to get into the pose after the buzz
        private const float MeasureSeconds = 1.6f;  // buzz + ~1 s of readings on the glove
        private const float ReplyTimeoutSeconds = 5f;

        /// True once calibration was offered this app session (pop-up shows only once).
        public static bool OfferedThisSession { get; set; }

        private HearSaySshMqttManager mqtt;
        private Step step = Step.Closed;
        private float stepStart, requestedAt;
        private readonly bool[] active = { true, true };       // gloves taking part
        private readonly bool[] replied = new bool[2];
        private readonly bool[,] fingerOk = new bool[2, 5];
        private readonly bool[] repliedBeforeFail = new bool[2];
        private int settleMs;
        private string failReason = "";
        private bool missingGloveOnly;
        private bool autoOpened;

        private static string Glove(int g) => HearSaySshMqttManager.GloveIds[g];
        private float Elapsed => Time.unscaledTime - stepStart;
        public bool IsOpen => step != Step.Closed;

        public void Open(HearSaySshMqttManager connection, bool automatic = false)
        {
            mqtt = connection;
            autoOpened = automatic;
            OfferedThisSession = true;
            Go(Step.Intro);
        }

        public void Close() => step = Step.Closed;

        private void Go(Step next)
        {
            step = next;
            stepStart = Time.unscaledTime;
            if (next == Step.Ready) { active[0] = active[1] = true; }
            switch (next)
            {
                case Step.Ready: Send("ready", 1500); break;
                case Step.Straight: Send("straight", SettleMs); break;
                case Step.Fist: Send("fist", SettleMs); break;
                case Step.Saving: Send("save", 0); break;
                case Step.Failed: Send("fail", 0); break;
            }
        }

        private static string StepName(Step s) => s switch
        {
            Step.Ready => "ready", Step.Straight => "straight", Step.Fist => "fist",
            Step.Saving => "save", _ => null,
        };

        private void Send(string stepName, int settleMs)
        {
            this.settleMs = settleMs;
            if (mqtt == null) return;
            requestedAt = Time.time;
            for (int g = 0; g < 2; g++)
            {
                replied[g] = false;
                for (int f = 0; f < 5; f++) fingerOk[g, f] = false;
                if (active[g]) mqtt.RequestCalibrationStep(Glove(g), stepName, settleMs);
            }
        }

        private float ExpectedSeconds => settleMs / 1000f + MeasureSeconds;

        private void Update()
        {
            if (mqtt == null) return;
            string name = StepName(step);
            if (name == null) return;

            bool all = true;
            for (int g = 0; g < 2; g++)
            {
                if (!active[g] || replied[g]) continue;
                if (mqtt.TryGetCalibrationStep(Glove(g), name, requestedAt, out var ack))
                {
                    replied[g] = true;
                    for (int f = 0; f < 5; f++) fingerOk[g, f] = ack.ok[f];
                }
                else all = false;
            }
            // Keep the countdown on screen for its full length so the wearer can follow it.
            if ((all && Elapsed >= ExpectedSeconds) || Elapsed >= ExpectedSeconds + ReplyTimeoutSeconds)
                Evaluate();
        }

        private void Evaluate()
        {
            failReason = "";
            missingGloveOnly = false;
            bool anyFingerBad = false, anyReplied = false;
            for (int g = 0; g < 2; g++)
            {
                if (!active[g]) continue;
                if (!replied[g])
                {
                    failReason += $"[X] {Glove(g)} did not reply - is it switched on and connected?\n";
                    continue;
                }
                anyReplied = true;
                string bad = "";
                for (int f = 0; f < 5; f++) if (!fingerOk[g, f]) bad += (bad.Length > 0 ? ", " : "") + FingerNames[f];
                if (bad.Length > 0)
                {
                    anyFingerBad = true;
                    failReason += $"[X] {Glove(g)}: {bad} - {Problem(step)}\n";
                }
            }

            if (failReason.Length == 0)
            {
                Go(step switch
                {
                    Step.Ready => Step.Straight, Step.Straight => Step.Fist,
                    Step.Fist => Step.Saving, _ => Step.Complete,
                });
                return;
            }
            // Only a glove that never answered at the Ready step: allow carrying on with the other one.
            missingGloveOnly = step == Step.Ready && anyReplied && !anyFingerBad;
            for (int g = 0; g < 2; g++) repliedBeforeFail[g] = active[g] && replied[g];
            Go(Step.Failed);
        }

        private static string Problem(Step s) => s switch
        {
            Step.Ready => "no reading or a very noisy reading (check the wire).",
            Step.Straight => "reading was not steady (keep the hand still).",
            Step.Fist => "did not bend enough (check the sensor sits on the finger).",
            _ => "could not save the calibration.",
        };

        // ---------------- Drawing (1000 x 650 GUI space) ----------------

        public void Draw()
        {
            if (step == Step.Closed) return;
            HearSayTheme.Card(new Rect(110, 90, 780, 470), HearSayTheme.Navy);
            HearSayTheme.Card(new Rect(110, 90, 780, 470), HearSayTheme.Panel);
            GUI.Label(new Rect(140, 105, 720, 30), "GLOVE CALIBRATION" + StepCounter(),
                HearSayTheme.Label(16, HearSayTheme.Accent));

            switch (step)
            {
                case Step.Intro:
                    Title(autoOpened ? "Calibrate your gloves before starting?" : "Calibrate the gloves");
                    Body("Takes about 15 seconds. The person wearing the gloves:\n" +
                         "1. Relaxes both hands\n2. Straightens all fingers and holds still\n" +
                         "3. Makes a comfortable fist and holds it\n" +
                         "The gloves buzz at the start of every step.");
                    bool connected = mqtt != null && mqtt.IsConnected;
                    if (!connected)
                        GUI.Label(new Rect(140, 420, 720, 34),
                            "Waiting for the Ultra96 connection (" + (mqtt != null ? mqtt.ConnectionStatus : "not started") + ")…",
                            HearSayTheme.Label(17, new Color(1f, 0.75f, 0.4f), TextAnchor.MiddleCenter));
                    if (HearSayTheme.Action(new Rect(150, 470, 330, 58), connected ? "Start" : "Start (not connected)") && connected)
                        Go(Step.Ready);
                    if (HearSayTheme.Action(new Rect(520, 470, 330, 58), "Skip")) Close();
                    break;

                case Step.Ready:
                    Instruction("Put on both gloves and relax your hands.", "One short buzz · checking the sensors…");
                    break;
                case Step.Straight:
                    Instruction("Straighten all fingers on both hands and hold still.", "One short buzz · measuring straight fingers");
                    break;
                case Step.Fist:
                    Instruction("Make a comfortable fist with both hands and hold.", "Two short buzzes · measuring bent fingers");
                    break;
                case Step.Saving:
                    Instruction("Saving the calibration…", "");
                    break;

                case Step.Complete:
                    Title("Calibration successful!");
                    Body("You can now use the gloves.\n\n" + GloveSummary() +
                         "\nThe gloves give one long buzz.");
                    if (HearSayTheme.Action(new Rect(150, 470, 330, 58), "Done")) Close();
                    if (HearSayTheme.Action(new Rect(520, 470, 330, 58), "Calibrate again")) Go(Step.Ready);
                    break;

                case Step.Failed:
                    Title("Some sensors did not respond correctly.");
                    GUI.Label(new Rect(140, 205, 720, 200), failReason + "\nPlease try again. (Three short buzzes.)",
                        HearSayTheme.Label(19, Color.white, TextAnchor.UpperLeft));
                    if (HearSayTheme.Action(new Rect(150, 470, 330, 58), "Try again")) Go(Step.Ready);
                    if (missingGloveOnly)
                    {
                        if (HearSayTheme.Action(new Rect(520, 470, 330, 58), "Continue with 1 glove"))
                        {
                            for (int g = 0; g < 2; g++) active[g] = repliedBeforeFail[g];
                            Go(Step.Straight);
                        }
                    }
                    else if (HearSayTheme.Action(new Rect(520, 470, 330, 58), "Close")) Close();
                    break;
            }
        }

        private string StepCounter() => step switch
        {
            Step.Ready => "  ·  STEP 1 OF 3", Step.Straight => "  ·  STEP 2 OF 3",
            Step.Fist => "  ·  STEP 3 OF 3", _ => "",
        };

        private string GloveSummary()
        {
            string text = "";
            for (int g = 0; g < 2; g++)
                text += active[g] ? $"[OK] {Glove(g)} calibrated\n" : $"[-] {Glove(g)} not used\n";
            return text;
        }

        private void Instruction(string text, string hint)
        {
            Title(text);
            if (hint.Length > 0) Body(hint);
            float left = ExpectedSeconds - Elapsed;
            string big = left > 0f ? Mathf.CeilToInt(left).ToString() : "…";
            GUI.Label(new Rect(140, 330, 720, 110), big, HearSayTheme.Label(72, HearSayTheme.Accent, TextAnchor.MiddleCenter));
            if (left <= 0f)
                GUI.Label(new Rect(140, 450, 720, 30), "Waiting for the gloves…",
                    HearSayTheme.Label(17, HearSayTheme.Muted, TextAnchor.MiddleCenter));
        }

        private void Title(string text) =>
            GUI.Label(new Rect(140, 140, 720, 60), text, HearSayTheme.Label(30, Color.white));

        private void Body(string text) =>
            GUI.Label(new Rect(140, 205, 720, 200), text, HearSayTheme.Label(21, HearSayTheme.Muted, TextAnchor.UpperLeft));
    }
}
