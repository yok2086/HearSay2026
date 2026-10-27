
using UnityEngine;

public class DummyServerManager : MonoBehaviour
{
    public string soundDirection = "None";
    public int speakerId = -1;
    public bool isSpeaking = false;
    public string transcript = "";
    public string aslWord = "";

    void Update()
    {
        // Fake microphone direction
        if (Input.GetKeyDown(KeyCode.LeftArrow))
        {
            soundDirection = "Left";
            Debug.Log("Dummy Server: Sound from LEFT");
        }

        if (Input.GetKeyDown(KeyCode.RightArrow))
        {
            soundDirection = "Right";
            Debug.Log("Dummy Server: Sound from RIGHT");
        }

        if (Input.GetKeyDown(KeyCode.UpArrow))
        {
            soundDirection = "Up";
            Debug.Log("Dummy Server: Sound from UP");
        }

        if (Input.GetKeyDown(KeyCode.DownArrow))
        {
            soundDirection = "Down";
            Debug.Log("Dummy Server: Sound from DOWN");
        }

        // Fake MediaPipe speaker detection
        if (Input.GetKeyDown(KeyCode.Alpha1))
        {
            speakerId = 1;
            isSpeaking = true;

            Debug.Log("Dummy Server: Person 1 is speaking");
        }

        // Fake Whisper
        if (Input.GetKeyDown(KeyCode.T))
        {
            transcript = "Hello";
            Debug.Log("Dummy Server: Whisper = Hello");
        }

        // Fake ASL model
        if (Input.GetKeyDown(KeyCode.A))
        {
            aslWord = "HELLO";
            Debug.Log("Dummy Server: ASL = HELLO");
        }

        // Reset everything
        if (Input.GetKeyDown(KeyCode.Space))
        {
            soundDirection = "None";
            speakerId = -1;
            isSpeaking = false;
            transcript = "";
            aslWord = "";

            Debug.Log("Dummy Server: Reset");
        }
    }
}