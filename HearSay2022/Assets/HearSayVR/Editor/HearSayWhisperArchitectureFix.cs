using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;

namespace HearSay.Editor
{
    // whisper.unity refuses any Android build that is not ARM64-only
    // (CheckAndroidPreprocessBuild, callbackOrder 0). HearSay also targets 32-bit
    // ARMv7 phones, where captions use the Ultra96 Whisper server instead (see
    // HearSayWhisperArmv7Stub.c). So: report ARM64 just for that check, then put
    // the real ARMv7 + ARM64 setting back before the player is actually built.
    internal sealed class HearSayWhisperArchBefore : IPreprocessBuildWithReport
    {
        internal static AndroidArchitecture? saved;
        public int callbackOrder => -100;

        public void OnPreprocessBuild(BuildReport report)
        {
            if (report.summary.platform != BuildTarget.Android) return;
            var arch = PlayerSettings.Android.targetArchitectures;
            if (arch == AndroidArchitecture.ARM64) return;
            saved = arch;
            PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
        }
    }

    internal sealed class HearSayWhisperArchAfter : IPreprocessBuildWithReport
    {
        public int callbackOrder => 100;

        public void OnPreprocessBuild(BuildReport report)
        {
            if (HearSayWhisperArchBefore.saved == null) return;
            PlayerSettings.Android.targetArchitectures = HearSayWhisperArchBefore.saved.Value;
            HearSayWhisperArchBefore.saved = null;
        }
    }
}
