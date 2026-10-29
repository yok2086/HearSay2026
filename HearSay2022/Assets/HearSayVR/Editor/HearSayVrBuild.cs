using System.IO;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.Android;
using UnityEditor.SceneManagement;

namespace HearSay.Editor
{
    public static class HearSayVrBuild
    {
        public const string Original = "Assets/MediaPipeUnity/Samples/Scenes/HearSay Demo/HearSay Demo.unity";
        public const string NormalHome = "Assets/MediaPipeUnity/Samples/Scenes/HearSay Demo/HearSay Start.unity";
        public const string VrScene = "Assets/HearSayVR/HearSay VR Test.unity";
        public const string VrHome = "Assets/HearSayVR/Final VR HearSay.unity";
        public const string SetupScene = "Assets/HearSayVR/HearSay VR Setup.unity";
        public const string VrStart = "Assets/HearSayVR/HearSay VR Start.unity";

        [MenuItem("HearSay/VR/Open VR Start scene")]
        public static void OpenVrStart()
        {
            if (EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo())
                EditorSceneManager.OpenScene(VrStart);
        }

        // File > Build Settings: HearSay VR Start first (the app opens in VR), plus the
        // scenes it loads. Other scenes stay listed but unticked. Use the normal list
        // again with "Use normal scenes in Build Settings".
        [MenuItem("HearSay/VR/Use VR scenes in Build Settings")]
        public static void UseVrScenes() { SetBuildScenes(VrStart, Original, NormalHome); }

        [MenuItem("HearSay/VR/Use normal scenes in Build Settings")]
        public static void UseNormalScenes() { SetBuildScenes(NormalHome, Original); }

        private static void SetBuildScenes(params string[] enabledInOrder)
        {
            var list = new System.Collections.Generic.List<EditorBuildSettingsScene>();
            foreach (var path in enabledInOrder) list.Add(new EditorBuildSettingsScene(path, true));
            foreach (var scene in EditorBuildSettings.scenes)
                if (System.Array.IndexOf(enabledInOrder, scene.path) < 0)
                    list.Add(new EditorBuildSettingsScene(scene.path, false));
            EditorBuildSettings.scenes = list.ToArray();
            AssetDatabase.SaveAssets();
            UnityEngine.Debug.Log("Build Settings scenes: " + string.Join(", ", enabledInOrder));
        }

        [MenuItem("HearSay/VR/Open VR setup")]
        public static void Open()
        {
            if (EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo())
                EditorSceneManager.OpenScene(SetupScene);
        }

        [MenuItem("HearSay/VR/Build separate VR test APK")]
        public static void BuildMenu()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            string path = EditorUtility.SaveFilePanel("Build VR test", "", "HearSay-VR-Test.apk", "apk");
            if (!string.IsNullOrEmpty(path)) Build(path);
        }

        public static void BuildBatch() { Build(Path.GetFullPath("Builds/HearSay-VR-Test.apk")); }

        // Builds and installs/launches on the phone connected by USB (no dialogs).
        [MenuItem("HearSay/VR/Build and run VR APK on phone")]
        public static void BuildAndRun()
        {
            if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
            Build(Path.GetFullPath("Builds/HearSay-VR-Test.apk"), BuildOptions.Development | BuildOptions.AutoRunPlayer);
        }

        private static void Build(string path) { Build(path, BuildOptions.Development); }

        private static void Build(string path, BuildOptions buildOptions)
        {
            if (EditorUserBuildSettings.activeBuildTarget != BuildTarget.Android)
                throw new BuildFailedException("Switch the project to Android before building the VR test.");
            string previousId = PlayerSettings.GetApplicationIdentifier(BuildTargetGroup.Android);
            string previousName = PlayerSettings.productName;
            try
            {
                // Install alongside the working APK instead of overwriting it.
                PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, previousId + ".vrtest");
                PlayerSettings.productName = "HearSay VR Test";
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    // Starts in HearSay VR Start (a copy of HearSay Start plus the
                    // split-screen object), which carries the two-lens view into
                    // HearSay Demo. The normal scenes themselves are unchanged.
                    scenes = new[] { VrStart, Original, NormalHome },
                    locationPathName = path,
                    target = BuildTarget.Android,
                    options = buildOptions
                });
                if (report.summary.result != BuildResult.Succeeded)
                    throw new BuildFailedException("VR test build failed; inspect the first Console error.");
            }
            finally
            {
                PlayerSettings.SetApplicationIdentifier(BuildTargetGroup.Android, previousId);
                PlayerSettings.productName = previousName;
                AssetDatabase.SaveAssets();
            }
        }
    }

    // The package's AAR is project-wide; its dependencies are required in normal
    // Android builds too. Only generated Gradle files are changed by this callback.
    public sealed class HearSayCardboardGradle : IPostGenerateGradleAndroidProject
    {
        public int callbackOrder => 10000;
        public void OnPostGenerateGradleAndroidProject(string path)
        {
            string gradle = Path.Combine(path, "build.gradle");
            string text = File.ReadAllText(gradle);
            if (!text.Contains("// HearSay Cardboard v1.24"))
            {
                text += "\n// HearSay Cardboard v1.24\ndependencies {\n" +
                    " implementation 'androidx.appcompat:appcompat:1.6.1'\n" +
                    " implementation 'com.google.android.gms:play-services-vision:20.1.3'\n" +
                    " implementation 'com.google.android.material:material:1.6.1'\n" +
                    " implementation 'com.google.protobuf:protobuf-javalite:3.19.4'\n}\n";
                File.WriteAllText(gradle, text);
            }
            string properties = Path.Combine(Directory.GetParent(path).FullName, "gradle.properties");
            string settings = File.Exists(properties) ? File.ReadAllText(properties) : "";
            settings = System.Text.RegularExpressions.Regex.Replace(settings,
                @"(?m)^android\.(useAndroidX|enableJetifier)=.*\r?\n?", "");
            File.WriteAllText(properties, settings + "\nandroid.useAndroidX=true\nandroid.enableJetifier=true\n");
        }
    }
}
