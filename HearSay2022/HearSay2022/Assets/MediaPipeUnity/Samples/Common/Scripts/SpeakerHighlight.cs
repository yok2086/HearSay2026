
using UnityEngine;
using UnityEngine.UI;

public class SpeakerHighlight : MonoBehaviour
{
    public RectTransform targetPerson;

    public float borderThickness = 5f;

    private Image top;
    private Image bottom;
    private Image left;
    private Image right;

    void Awake()
    {
        // Hide the highlight immediately when the app starts
        gameObject.SetActive(false);
    }

    void OnEnable()
    {
        // Recreate the border whenever the highlight is enabled
        CreateBorder();
    }

    void Update()
    {
        if (targetPerson == null)
            return;

        RectTransform highlight =
            GetComponent<RectTransform>();

        highlight.position = targetPerson.position;

        highlight.sizeDelta =
            targetPerson.sizeDelta +
            new Vector2(20f, 20f);

        UpdateBorder();
    }

    void CreateBorder()
    {
        // Don't create the border multiple times
        if (top != null)
            return;

        top = CreateSide("Top");
        bottom = CreateSide("Bottom");
        left = CreateSide("Left");
        right = CreateSide("Right");
    }

    Image CreateSide(string sideName)
    {
        GameObject obj =
            new GameObject(sideName);

        obj.transform.SetParent(transform);

        RectTransform rect =
            obj.AddComponent<RectTransform>();

        Image image =
            obj.AddComponent<Image>();

        image.color = Color.yellow;

        return image;
    }

    void UpdateBorder()
    {
        RectTransform rect =
            GetComponent<RectTransform>();

        float width = rect.rect.width;
        float height = rect.rect.height;

        SetSide(
            top,
            new Vector2(width, borderThickness),
            new Vector2(0f, height / 2f)
        );

        SetSide(
            bottom,
            new Vector2(width, borderThickness),
            new Vector2(0f, -height / 2f)
        );

        SetSide(
            left,
            new Vector2(borderThickness, height),
            new Vector2(-width / 2f, 0f)
        );

        SetSide(
            right,
            new Vector2(borderThickness, height),
            new Vector2(width / 2f, 0f)
        );
    }

    void SetSide(
        Image image,
        Vector2 size,
        Vector2 position
    )
    {
        RectTransform rect =
            image.GetComponent<RectTransform>();

        rect.sizeDelta = size;
        rect.anchoredPosition = position;
    }
}
