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

        [MenuItem("HearSay/Debug/Open VR app on phone")]
        public static void OpenApp()
        {
            string adb = Path.Combine(AndroidExternalToolsSettings.sdkRootPath, "platform-tools", "adb");
            var info = new ProcessStartInfo(adb,
                "shell monkey -p com.DefaultCompany.HearSay2022.vrtest -c android.intent.category.LAUNCHER 1")
            {
                RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false,
            };
            using (var process = Process.Start(info))
            {
                string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
                process.WaitForExit(20000);
                Directory.CreateDirectory("Builds");
                File.WriteAllText("Builds/phone-open.txt", output);
                UnityEngine.Debug.Log("HearSay: open app on phone: " + output);
            }
        }

        [MenuItem("HearSay/Debug/Check phone network")]
        public static void CheckNetwork()
        {
            string adb = Path.Combine(AndroidExternalToolsSettings.sdkRootPath, "platform-tools", "adb");
            string script = "echo '== active network =='; dumpsys connectivity | grep -E 'Active default network|NetworkAgentInfo.*(WIFI|CELLULAR|VPN)' | head -5; " +
                "echo '== wifi =='; dumpsys wifi | grep -m1 'mWifiInfo'; " +
                "echo '== stujump ssh banner =='; (sleep 4) | toybox nc -w 6 stujump.comp.nus.edu.sg 22 | head -c 80; echo; " +
                "echo '== https test =='; (echo -e 'HEAD / HTTP/1.0\\r\\n\\r\\n'; sleep 3) | toybox nc -w 5 www.google.com 80 | head -1";
            var info = new ProcessStartInfo(adb, "shell \"" + script.Replace("\"", "\\\"") + "\"")
            {
                RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false,
            };
            using (var process = Process.Start(info))
            {
                string output = process.StandardOutput.ReadToEnd() + process.StandardError.ReadToEnd();
                process.WaitForExit(30000);
                Directory.CreateDirectory("Builds");
                File.WriteAllText("Builds/phone-network.txt", output);
                UnityEngine.Debug.Log("HearSay: phone network check:\n" + output);
            }
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
