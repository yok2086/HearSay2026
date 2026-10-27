using UnityEngine;
using TMPro;

public class CaptionManager : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public TextMeshProUGUI captionText;

    [Header("Position")]
    public RectTransform targetPerson;

    public Vector2 captionOffset = new Vector2(0f, -230f);

    void Start()
    {
        ClearCaption();
    }

    void Update()
    {
        if (dummyServer == null)
            return;

        if (dummyServer.transcript != "")
        {
            ShowCaption(dummyServer.transcript);
        }
        else
        {
            ClearCaption();
        }

        UpdatePosition();
    }

    void ShowCaption(string text)
    {
        if (captionText != null)
        {
            captionText.text = text;
        }
    }

    void ClearCaption()
    {
        if (captionText != null)
        {
            captionText.text = "";
        }
    }

    void UpdatePosition()
    {
        if (captionText == null || targetPerson == null)
            return;

        RectTransform captionRect = captionText.GetComponent<RectTransform>();

        captionRect.position =
            targetPerson.position + (Vector3)captionOffset;
    }
}
