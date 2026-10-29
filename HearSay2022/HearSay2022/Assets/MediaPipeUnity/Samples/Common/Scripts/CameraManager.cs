using UnityEngine;
using UnityEngine.UI;

public class CameraManager : MonoBehaviour
{
    public RawImage cameraFeed;

    private WebCamTexture webcam;

    void Start()
    {
        webcam = new WebCamTexture();
        cameraFeed.texture = webcam;
        webcam.Play();
    }
}
