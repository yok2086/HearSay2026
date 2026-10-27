using UnityEngine;

public class MicPermissionTest : MonoBehaviour
{
    void Start()
    {
        Debug.Log("Microphones found:");

        foreach (string mic in Microphone.devices)
        {
            Debug.Log("MIC: " + mic);
        }

        if (Microphone.devices.Length > 0)
        {
            Debug.Log("Starting microphone...");
            AudioClip clip = Microphone.Start(Microphone.devices[0], true, 10, 44100);

            if (clip == null)
                Debug.LogError("MIC FAILED TO START");
            else
                Debug.Log("MIC STARTED");
        }
        else
        {
            Debug.LogError("NO MICROPHONES FOUND");
        }
    }
}