#if UNITY_EDITOR
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

[InitializeOnLoad]
internal static class CreateARBackCameraScene
{
    private const string SourceScene = "Assets/Scenes/HearSayVisualizer.unity";
    private const string DestinationScene = "Assets/Scenes/HearSay AR Back Camera.unity";

    static CreateARBackCameraScene()
    {
        EditorApplication.delayCall += CreateIfNeeded;
    }

    private static void CreateIfNeeded()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(DestinationScene) != null)
        {
            return;
        }

        var sourceScene = EditorSceneManager.OpenScene(SourceScene, OpenSceneMode.Additive);
        var destinationScene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Additive);

        CopyRoot(sourceScene, destinationScene, "AR Session");
        CopyRoot(sourceScene, destinationScene, "XRRig");

        EditorSceneManager.SaveScene(destinationScene, DestinationScene);
        EditorSceneManager.CloseScene(destinationScene, true);
        EditorSceneManager.CloseScene(sourceScene, true);
        Debug.Log("HearSay: created HearSay AR Back Camera scene.");
    }

    private static void CopyRoot(Scene sourceScene, Scene destinationScene, string rootName)
    {
        foreach (var root in sourceScene.GetRootGameObjects())
        {
            if (root.name != rootName)
            {
                continue;
            }

            var copy = Object.Instantiate(root);
            copy.name = root.name;
            SceneManager.MoveGameObjectToScene(copy, destinationScene);
            return;
        }

        Debug.LogError("HearSay: could not find " + rootName + " in the AR source scene.");
    }
}
#endif
