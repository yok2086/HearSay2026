
using UnityEngine;
using UnityEngine.UI;

public class SoundWaveRenderer : MonoBehaviour
{
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
        // LEFT
        if (Input.GetKeyDown(KeyCode.LeftArrow))
        {
            StartSound(
                Vector2.left,
                new Vector2(-startOffset, 0f)
            );
        }

        // RIGHT
        if (Input.GetKeyDown(KeyCode.RightArrow))
        {
            StartSound(
                Vector2.right,
                new Vector2(startOffset, 0f)
            );
        }

        // UP
        if (Input.GetKeyDown(KeyCode.UpArrow))
        {
            StartSound(
                Vector2.up,
                new Vector2(0f, startOffset)
            );
        }

        // DOWN
        if (Input.GetKeyDown(KeyCode.DownArrow))
        {
            StartSound(
                Vector2.down,
                new Vector2(0f, -startOffset)
            );
        }

        // SPACE = STOP
        if (Input.GetKeyDown(KeyCode.Space))
        {
            StopSound();
        }

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

            // Ripple 1 starts first
            // Ripple 2 follows
            // Ripple 3 follows
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

            // Smooth progress from 0 → 1
            float progress =
                Mathf.SmoothStep(0f, 1f, t);

            // --------------------------------
            // POSITION
            // --------------------------------

            float startingGap =
                i * rippleGap;

            Vector2 individualStart =
                startPosition +
                direction * startingGap;

            ripple.anchoredPosition =
                individualStart +
                direction *
                (rippleDistance * progress);

            // --------------------------------
            // GROW BIGGER
            // --------------------------------

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

            // --------------------------------
            // FADE OUT
            // --------------------------------

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
                ripple.gameObject.SetActive(false);
        }
    }
}
