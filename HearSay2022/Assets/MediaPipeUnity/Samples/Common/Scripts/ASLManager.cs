using UnityEngine;
using TMPro;

public class ASLManager : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public TextMeshProUGUI aslText;

    [Header("Position")]
    public RectTransform targetPerson;

    public Vector2 aslOffset = new Vector2(0f, -280f);

    void Start()
    {
        ClearASL();
    }

    void Update()
    {
        if (dummyServer == null)
            return;

        if (dummyServer.aslWord != "")
        {
            ShowASL(dummyServer.aslWord);
        }
        else
        {
            ClearASL();
        }

        UpdatePosition();
    }

    void ShowASL(string word)
    {
        if (aslText != null)
        {
            aslText.text = "ASL: " + word;
        }
    }

    void ClearASL()
    {
        if (aslText != null)
        {
            aslText.text = "";
        }
    }

    void UpdatePosition()
    {
        if (aslText == null || targetPerson == null)
            return;

        RectTransform aslRect = aslText.GetComponent<RectTransform>();

        aslRect.position =
            targetPerson.position + (Vector3)aslOffset;
    }
}
