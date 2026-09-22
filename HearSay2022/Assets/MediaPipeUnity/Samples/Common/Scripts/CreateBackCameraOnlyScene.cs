#if UNITY_EDITOR
using HearSay;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

[InitializeOnLoad]
internal static class CreateBackCameraOnlyScene
{
    private const string Folder = "Assets/MediaPipeUnity/Samples/Scenes/Back Camera Only";
    private const string ScenePath = Folder + "/Back Camera Only.unity";

    static CreateBackCameraOnlyScene()
    {
        EditorApplication.delayCall += CreateIfNeeded;
    }

    private static void CreateIfNeeded()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(ScenePath) != null)
        {
            return;
        }

        if (!AssetDatabase.IsValidFolder(Folder))
        {
            AssetDatabase.CreateFolder("Assets/MediaPipeUnity/Samples/Scenes", "Back Camera Only");
        }

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Additive);
        var cameraObject = new GameObject("Back Camera Only", typeof(AndroidCameraTest));
        var test = cameraObject.GetComponent<AndroidCameraTest>();
        var serializedTest = new SerializedObject(test);
        serializedTest.FindProperty("showStatus").boolValue = false;
        serializedTest.ApplyModifiedPropertiesWithoutUndo();

        EditorSceneManager.SaveScene(scene, ScenePath);
        EditorSceneManager.CloseScene(scene, true);
        Debug.Log("HearSay: created Back Camera Only scene.");
    }
}
#endif
