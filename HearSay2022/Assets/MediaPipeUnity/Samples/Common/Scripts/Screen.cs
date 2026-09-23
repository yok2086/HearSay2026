// Copyright (c) 2021 homuler
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

using UnityEngine;
using UnityEngine.UI;

namespace Mediapipe.Unity
{
  public class Screen : MonoBehaviour
  {
    [SerializeField] private RawImage _screen;

    private ImageSource _imageSource;

    public Texture texture
    {
      get => _screen.texture;
      set => _screen.texture = value;
    }

    // UI overlays that follow the phone camera should be parented here so they
    // inherit the preview's aspect ratio and rotation.
    public RectTransform overlayRectTransform => _screen.rectTransform;

    // Converts a MediaPipe normalized landmark (top-left image origin) into the
    // actual on-screen point of this camera preview, including its rotation and crop.
    public Vector2 NormalizedLandmarkToScreenPoint(Vector2 normalizedLandmark)
    {
      var rect = _screen.rectTransform;
      var localPoint = new Vector3(
        rect.rect.xMin + normalizedLandmark.x * rect.rect.width,
        rect.rect.yMax - normalizedLandmark.y * rect.rect.height,
        0f
      );
      // The preview uses a camera-space canvas, while captions use an overlay
      // canvas. Null is valid only for an overlay, not for the preview's world point.
      var canvas = _screen.canvas;
      var root = canvas != null ? canvas.rootCanvas : null;
      Camera renderCamera = null;
      if (root != null && root.renderMode != RenderMode.ScreenSpaceOverlay)
      {
        renderCamera = root.worldCamera != null ? root.worldCamera : Camera.main;
      }
      return RectTransformUtility.WorldToScreenPoint(renderCamera, rect.TransformPoint(localPoint));
    }

    public UnityEngine.Rect uvRect
    {
      set => _screen.uvRect = value;
    }

    public void Initialize(ImageSource imageSource)
    {
      _imageSource = imageSource;

      Resize(_imageSource.textureWidth, _imageSource.textureHeight);
      Rotate(_imageSource.rotation.Reverse());
      ResetUvRect(RunningMode.Async);
      texture = imageSource.GetCurrentTexture();
    }

    public void Resize(int width, int height)
    {
      _screen.rectTransform.sizeDelta = new Vector2(width, height);
    }

    public void Rotate(RotationAngle rotationAngle)
    {
      _screen.rectTransform.localEulerAngles = rotationAngle.GetEulerAngles();
    }

    public void ReadSync(Experimental.TextureFrame textureFrame)
    {
      if (!(texture is Texture2D))
      {
        texture = new Texture2D(_imageSource.textureWidth, _imageSource.textureHeight, TextureFormat.RGBA32, false);
        ResetUvRect(RunningMode.Sync);
      }
      textureFrame.CopyTexture(texture);
    }

    private void ResetUvRect(RunningMode runningMode)
    {
      var rect = new UnityEngine.Rect(0, 0, 1, 1);

      if (_imageSource.isVerticallyFlipped && runningMode == RunningMode.Async)
      {
        // In Async mode, we don't need to flip the screen vertically since the image will be copied on CPU.
        rect = FlipVertically(rect);
      }

      if (_imageSource.isFrontFacing)
      {
        // Flip the image (not the screen) horizontally.
        // It should be taken into account that the image will be rotated later.
        var rotation = _imageSource.rotation;

        if (rotation == RotationAngle.Rotation0 || rotation == RotationAngle.Rotation180)
        {
          rect = FlipHorizontally(rect);
        }
        else
        {
          rect = FlipVertically(rect);
        }
      }

      uvRect = rect;
    }

    private UnityEngine.Rect FlipHorizontally(UnityEngine.Rect rect)
    {
      return new UnityEngine.Rect(1 - rect.x, rect.y, -rect.width, rect.height);
    }

    private UnityEngine.Rect FlipVertically(UnityEngine.Rect rect)
    {
      return new UnityEngine.Rect(rect.x, 1 - rect.y, rect.width, -rect.height);
    }
  }
}
