using System.Diagnostics;
using System.IO;
using UnityEditor;
using UnityEditor.Android;

namespace HearSay.Editor
{
    // HearSay > Debug > Save phone log: dumps the connected phone's recent log
    // (adb logcat) to Builds/phone-log.txt so a frozen/crashing build can be checked.
    public static class HearSayPhoneLog
    {
        [MenuItem("HearSay/Debug/Save phone log")]
        public static void Save()
        {
            string sdk = AndroidExternalToolsSettings.sdkRootPath;
            string adb = Path.Combine(sdk, "platform-tools", "adb");
            Directory.CreateDirectory("Builds");
            string output = Path.GetFullPath("Builds/phone-log.txt");
            var info = new ProcessStartInfo(adb, "logcat -d -v time -t 6000")
            {
                RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false,
            };
            using (var process = Process.Start(info))
            {
                string log = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
                process.WaitForExit(20000);
                File.WriteAllText(output, log);
            }
            UnityEngine.Debug.Log("HearSay: phone log saved to " + output);
            Screenshot();
        }

        [MenuItem("HearSay/Debug/Save phone screenshot")]
        public static void Screenshot()
        {
            string adb = Path.Combine(AndroidExternalToolsSettings.sdkRootPath, "platform-tools", "adb");
            Directory.CreateDirectory("Builds");
            string output = Path.GetFullPath("Builds/phone-screen.png");
            var info = new ProcessStartInfo(adb, "exec-out screencap -p")
            {
                RedirectStandardOutput = true, UseShellExecute = false,
            };
            using (var process = Process.Start(info))
            using (var file = File.Create(output))
            {
                process.StandardOutput.BaseStream.CopyTo(file);
                process.WaitForExit(20000);
            }
            UnityEngine.Debug.Log("HearSay: phone screenshot saved to " + output);
        }
    }
}
