#if UNITY_EDITOR
using HearSay;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

// Creates a separate combined test scene after Unity recompiles this script.
// It never edits the original Image Segmentation scene.
[InitializeOnLoad]
internal static class CreateHearSayCombinedTestScene
{
    private const string SourceScene = "Assets/MediaPipeUnity/Samples/Scenes/Image Segmentation/Image Segmentation.unity";
    private const string DestinationFolder = "Assets/MediaPipeUnity/Samples/Scenes/HearSay Combined Test";
    private const string DestinationScene = DestinationFolder + "/HearSay Combined Test.unity";

    static CreateHearSayCombinedTestScene()
    {
        EditorApplication.delayCall += CreateSceneIfNeeded;
    }

    private static void CreateSceneIfNeeded()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(DestinationScene) != null)
        {
            return;
        }

        if (!AssetDatabase.IsValidFolder(DestinationFolder))
        {
            AssetDatabase.CreateFolder("Assets/MediaPipeUnity/Samples/Scenes", "HearSay Combined Test");
        }

        if (!AssetDatabase.CopyAsset(SourceScene, DestinationScene))
        {
            Debug.LogError("HearSay: could not create the combined test scene.");
            return;
        }

        AssetDatabase.SaveAssets();
        var scene = EditorSceneManager.OpenScene(DestinationScene, OpenSceneMode.Additive);
        var captions = FindComponentInScene<HearSayLiveCaption>(scene);
        if (captions == null)
        {
            Debug.LogError("HearSay: the copied scene has no HearSayLiveCaption component.");
            EditorSceneManager.CloseScene(scene, true);
            return;
        }

        var serializedCaptions = new SerializedObject(captions);
        serializedCaptions.FindProperty("startOnlyWhenSpeakerDetected").boolValue = true;
        serializedCaptions.ApplyModifiedPropertiesWithoutUndo();

        EditorSceneManager.MarkSceneDirty(scene);
        EditorSceneManager.SaveScene(scene);
        EditorSceneManager.CloseScene(scene, true);
        Debug.Log("HearSay: created HearSay Combined Test. It starts captions automatically when a speaker is highlighted.");
    }

    private static T FindComponentInScene<T>(Scene scene) where T : Component
    {
        foreach (var root in scene.GetRootGameObjects())
        {
            var component = root.GetComponentInChildren<T>(true);
            if (component != null)
            {
                return component;
            }
        }

        return null;
    }
}
#endif
