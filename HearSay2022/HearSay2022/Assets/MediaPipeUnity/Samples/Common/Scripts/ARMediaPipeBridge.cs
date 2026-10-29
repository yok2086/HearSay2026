using UnityEngine;
using UnityEngine.XR.ARFoundation;
using UnityEngine.XR.ARSubsystems;

public class ARMediaPipeBridge : MonoBehaviour
{
    [SerializeField]
    private ARCameraManager arCameraManager;

    private Texture2D cameraTexture;

    public Texture2D CameraTexture
    {
        get { return cameraTexture; }
    }

    void Update()
    {
        if (arCameraManager == null)
            return;

        if (!arCameraManager.TryAcquireLatestCpuImage(out XRCpuImage cpuImage))
            return;
        Debug.Log("AR camera frame received");

        using (cpuImage)
        {
            var conversionParams = new XRCpuImage.ConversionParams
            {
                inputRect = new RectInt(
                    0,
                    0,
                    cpuImage.width,
                    cpuImage.height
                ),

                outputDimensions = new Vector2Int(
                    cpuImage.width,
                    cpuImage.height
                ),

                outputFormat = TextureFormat.RGBA32,

                transformation = XRCpuImage.Transformation.None
            };

            if (cameraTexture == null ||
                cameraTexture.width != cpuImage.width ||
                cameraTexture.height != cpuImage.height)
            {
                if (cameraTexture != null)
                {
                    Destroy(cameraTexture);
                }

                cameraTexture = new Texture2D(
                    cpuImage.width,
                    cpuImage.height,
                    TextureFormat.RGBA32,
                    false
                );
            }

            cpuImage.Convert(
                conversionParams,
                cameraTexture.GetRawTextureData<byte>()
            );

            cameraTexture.Apply();
        }
    }

    void OnDestroy()
    {
        if (cameraTexture != null)
        {
            Destroy(cameraTexture);
            cameraTexture = null;
        }
    }
}
