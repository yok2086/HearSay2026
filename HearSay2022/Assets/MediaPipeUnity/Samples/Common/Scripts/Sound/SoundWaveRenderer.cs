
using UnityEngine;
using UnityEngine.UI;

public class SoundWaveRenderer : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public RectTransform[] ripples;

    [Header("Ripple Movement")]
    public float rippleDistance = 120f;
    public float duration = 1.2f;
    public float rippleGap = 60f;

    [Header("Ripple Size")]
    public float startSize = 50f;
    public float endSize = 160f;

    [Header("Sound Duration")]
    public float soundDuration = 5f;

    [Header("Position")]
    public float startOffset = 180f;

    [Header("Ripple Delay")]
    public float rippleDelay = 0.25f;

    private bool isPlaying = false;
    private string lastDirection = "None";

    private float soundTimer = 0f;
    private float rippleTimer = 0f;

    private Vector2 direction;
    private Vector2 startPosition;

    void Start()
    {
        HideRipples();
    }

    void Update()
    {
        // Make sure DummyServerManager is connected
        if (dummyServer == null)
            return;

        string currentDirection = dummyServer.soundDirection;

        // Only start a new ripple when the direction changes
        if (currentDirection != lastDirection)
        {
            lastDirection = currentDirection;

            if (currentDirection == "Left")
            {
                StartSound(
                    Vector2.left,
                    new Vector2(-startOffset, 0f)
                );
            }
            else if (currentDirection == "Right")
            {
                StartSound(
                    Vector2.right,
                    new Vector2(startOffset, 0f)
                );
            }
            else if (currentDirection == "Up")
            {
                StartSound(
                    Vector2.up,
                    new Vector2(0f, startOffset)
                );
            }
            else if (currentDirection == "Down")
            {
                StartSound(
                    Vector2.down,
                    new Vector2(0f, -startOffset)
                );
            }
            else if (currentDirection == "None")
            {
                StopSound();
            }
        }

        // Nothing to animate
        if (!isPlaying)
            return;

        soundTimer += Time.deltaTime;

        // Stop after sound duration
        if (soundTimer >= soundDuration)
        {
            StopSound();
            return;
        }

        rippleTimer += Time.deltaTime;

        AnimateRipples();
    }

    void AnimateRipples()
    {
        for (int i = 0; i < ripples.Length; i++)
        {
            RectTransform ripple = ripples[i];

            if (ripple == null)
                continue;

            // Each ripple starts after the previous one
            float t =
                (rippleTimer - i * rippleDelay)
                / duration;

            // Not started yet
            if (t < 0f)
            {
                ripple.gameObject.SetActive(false);
                continue;
            }

            // Finished
            if (t > 1f)
            {
                ripple.gameObject.SetActive(false);
                continue;
            }

            ripple.gameObject.SetActive(true);

            // Smooth animation
            float progress =
                Mathf.SmoothStep(0f, 1f, t);

            // Starting position for this ripple
            float startingGap =
                i * rippleGap;

            Vector2 individualStart =
                startPosition +
                direction * startingGap;

            // Move outward
            ripple.anchoredPosition =
                individualStart +
                direction *
                (rippleDistance * progress);

            // Grow
            float size =
                Mathf.Lerp(
                    startSize,
                    endSize,
                    progress
                );

            ripple.SetSizeWithCurrentAnchors(
                RectTransform.Axis.Horizontal,
                size
            );

            ripple.SetSizeWithCurrentAnchors(
                RectTransform.Axis.Vertical,
                size
            );

            // Fade out
            Image image =
                ripple.GetComponent<Image>();

            if (image != null)
            {
                Color color = image.color;

                color.a =
                    Mathf.Lerp(
                        0.9f,
                        0f,
                        progress
                    );

                image.color = color;
            }
        }
    }

    void StartSound(
        Vector2 newDirection,
        Vector2 newStartPosition
    )
    {
        direction = newDirection;
        startPosition = newStartPosition;

        soundTimer = 0f;
        rippleTimer = 0f;

        isPlaying = true;

        HideRipples();
    }

    void StopSound()
    {
        isPlaying = false;

        soundTimer = 0f;
        rippleTimer = 0f;

        HideRipples();
    }

    void HideRipples()
    {
        foreach (RectTransform ripple in ripples)
        {
            if (ripple != null)
            {
                ripple.gameObject.SetActive(false);
            }
        }
    }
}
