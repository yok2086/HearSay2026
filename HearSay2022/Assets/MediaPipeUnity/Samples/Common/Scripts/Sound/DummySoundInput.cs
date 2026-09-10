using UnityEngine;

public class DummySoundInput : MonoBehaviour
{
    public enum SoundDirection
    {
        None,
        Left,
        Right,
        Up,
        Down
    }

    public SoundDirection CurrentDirection { get; private set; } = SoundDirection.None;
    public bool SoundPresent { get; private set; } = false;

    void Update()
    {
        if (Input.GetKeyDown(KeyCode.LeftArrow))
            SetSound(SoundDirection.Left);

        if (Input.GetKeyDown(KeyCode.RightArrow))
            SetSound(SoundDirection.Right);

        if (Input.GetKeyDown(KeyCode.UpArrow))
            SetSound(SoundDirection.Up);

        if (Input.GetKeyDown(KeyCode.DownArrow))
            SetSound(SoundDirection.Down);

        if (Input.GetKeyDown(KeyCode.Space))
            StopSound();
    }

    void SetSound(SoundDirection direction)
    {
        CurrentDirection = direction;
        SoundPresent = true;

        Debug.Log("Sound detected: " + direction);
    }

    void StopSound()
    {
        CurrentDirection = SoundDirection.None;
        SoundPresent = false;

        Debug.Log("No sound");
    }
}