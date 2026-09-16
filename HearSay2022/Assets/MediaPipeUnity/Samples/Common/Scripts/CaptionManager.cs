
using UnityEngine;
using TMPro;

public class CaptionManager : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public TextMeshProUGUI captionText;

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
}
