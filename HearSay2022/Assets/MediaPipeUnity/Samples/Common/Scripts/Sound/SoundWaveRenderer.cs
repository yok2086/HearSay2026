using TMPro;
using UnityEngine;

public class SoundWaveRenderer : MonoBehaviour
{
    public DummySoundInput soundInput;
    public RectTransform waveTransform;
    public TMP_Text waveText;

    public float pulseSpeed = 8f;
    public float minScale = 0.8f;
    public float maxScale = 1.2f;

    public float sideOffset = 350f;
    public float verticalOffset = 200f;

    public float soundDuration = 3f;

    private float soundTimer = 0f;

    void Start()
    {
        waveText.gameObject.SetActive(false);
    }

    void Update()
    {
        if (soundInput == null || waveText == null || waveTransform == null)
            return;

        // Arrow key starts a new 5-second sound
        if (Input.GetKeyDown(KeyCode.LeftArrow) ||
            Input.GetKeyDown(KeyCode.RightArrow) ||
            Input.GetKeyDown(KeyCode.UpArrow) ||
            Input.GetKeyDown(KeyCode.DownArrow))
        {
            soundTimer = 0f;
        }

        // Space stops the sound
        if (!soundInput.SoundPresent)
        {
            waveText.gameObject.SetActive(false);
            return;
        }

        // Count up the timer
        soundTimer += Time.deltaTime;

        // Hide after 5 seconds
        if (soundTimer >= soundDuration)
        {
            waveText.gameObject.SetActive(false);
            return;
        }

        // Show the wave
        waveText.gameObject.SetActive(true);

        // Choose direction
        switch (soundInput.CurrentDirection)
        {
            case DummySoundInput.SoundDirection.Left:

                waveText.text = ")))";

                waveTransform.anchoredPosition =
                    new Vector2(-sideOffset, 0);

                break;


            case DummySoundInput.SoundDirection.Right:

                waveText.text = "(((";

                waveTransform.anchoredPosition =
                    new Vector2(sideOffset, 0);

                break;


            case DummySoundInput.SoundDirection.Up:

                waveText.text = ")))";

                waveTransform.anchoredPosition =
                    new Vector2(0, verticalOffset);

                break;


            case DummySoundInput.SoundDirection.Down:

                waveText.text = "(((";

                waveTransform.anchoredPosition =
                    new Vector2(0, -verticalOffset);

                break;
        }

        // Pulsing animation
        float pulse =
            (Mathf.Sin(Time.time * pulseSpeed) + 1f) / 2f;

        float scale =
            Mathf.Lerp(minScale, maxScale, pulse);

        waveTransform.localScale =
            Vector3.one * scale;
    }
}