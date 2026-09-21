#if UNITY_EDITOR
using HearSay;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

[InitializeOnLoad]
internal static class CreateAndroidCameraTestScene
{
    private const string Folder = "Assets/MediaPipeUnity/Samples/Scenes/Android Camera Test";
    private const string ScenePath = Folder + "/Android Camera Test.unity";

    static CreateAndroidCameraTestScene()
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
            AssetDatabase.CreateFolder("Assets/MediaPipeUnity/Samples/Scenes", "Android Camera Test");
        }

        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Additive);
        new GameObject("Android Camera Test", typeof(AndroidCameraTest));
        EditorSceneManager.SaveScene(scene, ScenePath);
        EditorSceneManager.CloseScene(scene, true);
        Debug.Log("HearSay: created Android Camera Test scene.");
    }
}
#endif
