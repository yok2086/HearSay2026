using UnityEngine;
using UnityEngine.UI;

public class RippleEffect : MonoBehaviour
{
    [Header("Ripple Objects")]
    public RectTransform[] ripples;

    [Header("Animation")]
    public float duration = 1.2f;
    public float startSize = 80f;
    public float endSize = 250f;
    public float spacing = 0.2f;

    [Header("Direction")]
    public float distance = 250f;

    private float timer;
    private bool isPlaying;
    private Vector2 direction;

    void Start()
    {
        HideRipples();
    }

    void Update()
    {
        // Start a new ripple when an arrow is pressed
        if (Input.GetKeyDown(KeyCode.LeftArrow))
        {
            StartRipple(Vector2.left);
        }

        if (Input.GetKeyDown(KeyCode.RightArrow))
        {
            StartRipple(Vector2.right);
        }

        if (Input.GetKeyDown(KeyCode.UpArrow))
        {
            StartRipple(Vector2.up);
        }

        if (Input.GetKeyDown(KeyCode.DownArrow))
        {
            StartRipple(Vector2.down);
        }

        // Stop immediately with Space
        if (Input.GetKeyDown(KeyCode.Space))
        {
            StopRipple();
        }

        if (!isPlaying)
            return;

        timer += Time.deltaTime;

        for (int i = 0; i < ripples.Length; i++)
        {
            float startTime = i * spacing;
            float t = (timer - startTime) / duration;

            if (t >= 0f && t <= 1f)
            {
                RectTransform ripple = ripples[i];

                ripple.gameObject.SetActive(true);

                // Expand
                float size = Mathf.Lerp(startSize, endSize, t);
                ripple.sizeDelta = new Vector2(size, size);

                // Move in sound direction
                Vector2 position =
                    direction * distance * t;

                ripple.anchoredPosition = position;

                // Fade out
                Image image = ripple.GetComponent<Image>();

                if (image != null)
                {
                    Color color = image.color;
                    color.a = Mathf.Lerp(0.8f, 0f, t);
                    image.color = color;
                }
            }
            else if (t > 1f)
            {
                ripples[i].gameObject.SetActive(false);
            }
        }

        // Finish animation
        if (timer >= duration + (spacing * ripples.Length))
        {
            StopRipple();
        }
    }

    void StartRipple(Vector2 newDirection)
    {
        direction = newDirection;
        timer = 0f;
        isPlaying = true;

        ResetRipples();
    }

    void StopRipple()
    {
        isPlaying = false;
        HideRipples();
    }

    void HideRipples()
    {
        foreach (RectTransform ripple in ripples)
        {
            ripple.gameObject.SetActive(false);
        }
    }

    void ResetRipples()
    {
        foreach (RectTransform ripple in ripples)
        {
            ripple.gameObject.SetActive(false);
            ripple.anchoredPosition = Vector2.zero;
            ripple.sizeDelta = new Vector2(startSize, startSize);

            Image image = ripple.GetComponent<Image>();

            if (image != null)
            {
                Color color = image.color;
                color.a = 0.8f;
                image.color = color;
            }
        }
    }
}