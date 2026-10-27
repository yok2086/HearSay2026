using UnityEngine;

public class SpeakerManager : MonoBehaviour
{
    public DummyServerManager dummyServer;

    public GameObject fakePerson;
    public GameObject speakerHighlight;

    void Start()
    {
        // Make sure both are hidden when the app starts
        if (fakePerson != null)
            fakePerson.SetActive(false);

        if (speakerHighlight != null)
            speakerHighlight.SetActive(false);
    }

    void Update()
    {
        if (dummyServer == null)
            return;

        // Person detected / speaking
        if (dummyServer.isSpeaking)
        {
            if (fakePerson != null)
                fakePerson.SetActive(true);

            if (speakerHighlight != null)
                speakerHighlight.SetActive(true);
        }
        else
        {
            if (fakePerson != null)
                fakePerson.SetActive(false);

            if (speakerHighlight != null)
                speakerHighlight.SetActive(false);
        }
    }
}
