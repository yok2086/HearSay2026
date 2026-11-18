using System;
using System.Collections;
using System.IO;
using UnityEngine;
using UnityEngine.Networking;
using SherpaOnnx;

namespace HearSay
{
    // On-phone streaming speech recognition with sherpa-onnx (k2-fsa), the alternative to
    // Whisper used by the "HearSay Demo Sherpa" scene. Words come out while the person is
    // still speaking, with no Ultra96 / SSH round trip.
    //
    // Files:
    //   Assets/Plugins/Sherpa/sherpa-onnx.dll                      C# API (NuGet org.k2fsa.sherpa.onnx)
    //   Assets/Plugins/Android/libs/<abi>/libsherpa-onnx-c-api.so  native engine
    //   Assets/Plugins/Android/libs/<abi>/libonnxruntime.so
    //   Assets/StreamingAssets/sherpa/<model>/...                  streaming zipformer model
    //
    // Plain class (not a MonoBehaviour), so it adds no serialized fields to the scene.
    public sealed class HearSaySherpa : IDisposable
    {
        public const string SceneName = "HearSay Demo Sherpa";
        public const string ModelDir = "sherpa/sherpa-onnx-streaming-zipformer-en-2023-06-26";
        // Smaller int8 encoder/joiner (fast on phones), full-precision decoder.
        public const string EncoderFile = "encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx";
        public const string DecoderFile = "decoder-epoch-99-avg-1-chunk-16-left-128.onnx";
        public const string JoinerFile = "joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx";
        public const string TokensFile = "tokens.txt";

        /// True in the separate "HearSay Sherpa" app (built with HearSay > VR > Build and run Sherpa APK).
        public static bool IsSherpaApp => Application.identifier.EndsWith(".sherpa");

        /// false: the Sherpa app sends speech to the Ultra96 (run sherpa_local/run_server.sh
        /// there). true: sherpa-onnx runs inside the app on the phone.
        public static bool RunOnPhone = true;

        /// With RunOnPhone: also send each finished sentence to Whisper on the Ultra96 and
        /// replace sherpa's text with Whisper's (more accurate) version when it comes back.
        /// sherpa's live words are shown first, so this never slows the captions down.
        public static bool CorrectWithUltra96 = false;

        /// Loudness needed to start listening (RMS, 0-1; 0 = off). Background noise below it
        /// is treated as silence. Once speech opens the gate it stays open for GateHoldSeconds,
        /// so quiet syllables and word endings are kept. Raise it if room noise still becomes
        /// words; lower it if the speaker has to talk too loudly.
        public static float NoiseGate = 0.025f;
        public static float GateHoldSeconds = 0.8f;
        /// Auto threshold: the app keeps measuring the room noise (the quietest moment of
        /// every 2 s, smoothed) and sets the gate GateAboveNoise times above it, so it rises
        /// by itself in a louder room. NoiseGate is then the lowest it goes, GateMax the highest.
        public static bool AutoGate = true;
        public static float GateAboveNoise = 3f;
        public static float GateMax = 0.2f;

        /// Shows the mic level meter (top-left of the caption screen) for setting NoiseGate.
        public static bool ShowMicMeter = true;

        public static bool SceneActive =>
            UnityEngine.SceneManagement.SceneManager.GetActiveScene().name == SceneName;

        // Words the model often mishears -> what was said (whole words, any capitals).
        // Same idea as sherpa_local/fixes.txt on the Ultra96. Add more pairs here.
        private static readonly (string wrong, string right)[] Fixes =
        {
            ("hullo", "hello"), ("hulo", "hello"), ("helo", "hello"), ("hallo", "hello"),
            ("here say", "HearSay"), ("hear say", "HearSay"),
        };

        /// Applies the Fixes list to a caption, keeping a capital first letter.
        public static string Fix(string text)
        {
            foreach (var (wrong, right) in Fixes)
                text = System.Text.RegularExpressions.Regex.Replace(text,
                    @"\b" + System.Text.RegularExpressions.Regex.Escape(wrong) + @"\b", right,
                    System.Text.RegularExpressions.RegexOptions.IgnoreCase);
            return text.Length > 0 ? char.ToUpperInvariant(text[0]) + text.Substring(1) : text;
        }

        private OnlineRecognizer recognizer;
        private OnlineStream stream;
        private string lastPartial = "";

        /// Text of the sentence being spoken right now (updates while speaking).
        public string Partial => lastPartial;

        /// Copies the model out of the APK (native code needs real files) and loads it.
        /// Calls done(recognizer, null) on success or done(null, error).
        public static IEnumerator Create(Action<HearSaySherpa, string> done)
        {
            string target = Path.Combine(Application.persistentDataPath, ModelDir);
            Directory.CreateDirectory(target);
            foreach (string file in new[] { EncoderFile, DecoderFile, JoinerFile, TokensFile })
            {
                string dst = Path.Combine(target, file);
                if (File.Exists(dst) && new FileInfo(dst).Length > 0) continue;
                string src = Application.streamingAssetsPath + "/" + ModelDir + "/" + file;
                if (!src.Contains("://")) src = "file://" + src; // editor / desktop
                using (var request = UnityWebRequest.Get(src))
                {
                    yield return request.SendWebRequest();
                    if (request.result != UnityWebRequest.Result.Success)
                    {
                        done(null, "Could not read " + file + " from the app: " + request.error);
                        yield break;
                    }
                    File.WriteAllBytes(dst, request.downloadHandler.data);
                }
            }

            HearSaySherpa sherpa = null;
            string error = null;
            try
            {
                var config = new OnlineRecognizerConfig();
                config.FeatConfig.SampleRate = 16000;
                config.FeatConfig.FeatureDim = 80;
                config.ModelConfig.Transducer.Encoder = Path.Combine(target, EncoderFile);
                config.ModelConfig.Transducer.Decoder = Path.Combine(target, DecoderFile);
                config.ModelConfig.Transducer.Joiner = Path.Combine(target, JoinerFile);
                config.ModelConfig.Tokens = Path.Combine(target, TokensFile);
                config.ModelConfig.Provider = "cpu";
                config.ModelConfig.NumThreads = 4;
                config.ModelConfig.Debug = 0;
                // Beam search keeps the 4 best guesses instead of 1: more accurate, still real time.
                config.DecodingMethod = "modified_beam_search";
                config.MaxActivePaths = 4;
                // Endpoint = end of a sentence: lets the caption settle and start fresh.
                config.EnableEndpoint = 1;
                config.Rule1MinTrailingSilence = 2.4f;  // nothing said yet: give up after 2.4 s
                config.Rule2MinTrailingSilence = 0.8f;  // after speech: 0.8 s pause ends the sentence
                config.Rule3MinUtteranceLength = 15f;   // never let one sentence run past 15 s

                sherpa = new HearSaySherpa();
                sherpa.recognizer = new OnlineRecognizer(config);
                sherpa.stream = sherpa.recognizer.CreateStream();
            }
            catch (Exception e)
            {
                error = "sherpa-onnx could not start: " + e.Message;
                Debug.LogException(e);
                sherpa?.Dispose();
                sherpa = null;
            }
            done(sherpa, error);
        }

        /// Feeds microphone audio. Returns true when the text changed; finished is true when
        /// the sentence just ended (the next call starts a new one).
        public bool Accept(int sampleRate, float[] samples, out string text, out bool finished)
        {
            stream.AcceptWaveform(sampleRate, samples);
            while (recognizer.IsReady(stream)) recognizer.Decode(stream);

            text = (recognizer.GetResult(stream).Text ?? "").Trim();
            finished = false;
            if (recognizer.IsEndpoint(stream))
            {
                finished = text.Length > 0;
                recognizer.Reset(stream);
            }
            bool changed = text != lastPartial;
            lastPartial = finished ? "" : text;
            return changed;
        }

        public void Dispose()
        {
            stream?.Dispose();
            recognizer?.Dispose();
            stream = null;
            recognizer = null;
        }
    }
}
