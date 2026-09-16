using UnityEngine;
using TMPro;

public class ASLManager : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public TextMeshProUGUI aslText;

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
}