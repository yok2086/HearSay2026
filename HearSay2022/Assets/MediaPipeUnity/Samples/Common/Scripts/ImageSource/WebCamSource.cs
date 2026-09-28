// Copyright (c) 2021 homuler
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;

#if UNITY_ANDROID
using UnityEngine.Android;
#endif

namespace Mediapipe.Unity
{
  public class WebCamSource : ImageSource
  {
    // Set by a scene-specific helper before the source is initialized.
    public static bool PreferFrontCamera { get; set; }
    private readonly int _preferableDefaultWidth = 1280;

    private const string _TAG = nameof(WebCamSource);

    private readonly ResolutionStruct[] _defaultAvailableResolutions;

    public WebCamSource(int preferableDefaultWidth, ResolutionStruct[] defaultAvailableResolutions)
    {
      _preferableDefaultWidth = preferableDefaultWidth;
      _defaultAvailableResolutions = defaultAvailableResolutions;
    }

    private static readonly object _PermissionLock = new object();
    private static bool _IsPermitted = false;

    private WebCamTexture _webCamTexture;
    private bool _isStarting;
    private bool _hasFrame;
    private WebCamTexture webCamTexture
    {
      get => _webCamTexture;
      set
      {
        if (_webCamTexture != null)
        {
          _webCamTexture.Stop();
        }
        _webCamTexture = value;
      }
    }

    public override int textureWidth => !isPrepared ? 0 : webCamTexture.width;
    public override int textureHeight => !isPrepared ? 0 : webCamTexture.height;

    public override bool isVerticallyFlipped => isPrepared && webCamTexture.videoVerticallyMirrored;
    public override bool isFrontFacing => isPrepared && (webCamDevice is WebCamDevice valueOfWebCamDevice) && valueOfWebCamDevice.isFrontFacing;
    public override RotationAngle rotation => !isPrepared ? RotationAngle.Rotation0 : (RotationAngle)webCamTexture.videoRotationAngle;

    private WebCamDevice? _webCamDevice;
    private WebCamDevice? webCamDevice
    {
      get => _webCamDevice;
      set
      {
        if (_webCamDevice is WebCamDevice valueOfWebCamDevice)
        {
          if (value is WebCamDevice valueOfValue && valueOfValue.name == valueOfWebCamDevice.name)
          {
            // not changed
            return;
          }
        }
        else if (value == null)
        {
          // not changed
          return;
        }
        _webCamDevice = value;
        resolution = GetDefaultResolution();
      }
    }
    public override string sourceName => (webCamDevice is WebCamDevice valueOfWebCamDevice) ? valueOfWebCamDevice.name : null;

    private WebCamDevice[] _availableSources;
    private WebCamDevice[] availableSources
    {
      get
      {
        if (_availableSources == null)
        {
          _availableSources = WebCamTexture.devices;
        }

        return _availableSources;
      }
      set => _availableSources = value;
    }

    public override string[] sourceCandidateNames => availableSources?.Select(device => device.name).ToArray();

#pragma warning disable IDE0025
    public override ResolutionStruct[] availableResolutions
    {
      get
      {
#if (UNITY_ANDROID || UNITY_IOS) && !UNITY_EDITOR
        if (webCamDevice is WebCamDevice valueOfWebCamDevice) {
          var supported = valueOfWebCamDevice.availableResolutions;
          if (supported != null && supported.Length > 0)
            return supported.Select(resolution => new ResolutionStruct(resolution)).ToArray();
          return _defaultAvailableResolutions;
        }
#endif
        return webCamDevice == null ? null : _defaultAvailableResolutions;
      }
    }
#pragma warning restore IDE0025

    public override bool isPrepared => webCamTexture != null && _hasFrame;
    public override bool isPlaying => webCamTexture != null && webCamTexture.isPlaying;

    private IEnumerator Initialize()
    {
      yield return GetPermission();

      if (!_IsPermitted)
      {
        yield break;
      }

      if (webCamDevice != null)
      {
        yield break;
      }

      availableSources = WebCamTexture.devices;

      if (availableSources != null && availableSources.Length > 0)
      {
        var selectedDevice = availableSources[0];
#if UNITY_ANDROID && !UNITY_EDITOR
        // Android devices can expose several lenses. The HearSay Demo requests the selfie
        // camera; other scenes keep the default back-camera preference.
        for (var i = 0; i < availableSources.Length; i++)
        {
          if (availableSources[i].isFrontFacing == PreferFrontCamera)
          {
            selectedDevice = availableSources[i];
            break;
          }
        }
#elif !UNITY_ANDROID
        // Preserve the desktop default unless a scene explicitly requests the selfie camera.
        if (PreferFrontCamera)
        {
          for (var i = 0; i < availableSources.Length; i++)
          {
            if (availableSources[i].isFrontFacing)
            {
              selectedDevice = availableSources[i];
              break;
            }
          }
        }
#endif
        webCamDevice = selectedDevice;
      }
    }

    private IEnumerator GetPermission()
    {
      lock (_PermissionLock)
      {
        if (_IsPermitted)
        {
          yield break;
        }

#if UNITY_ANDROID
        if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
        {
          var permissionRequestFinished = false;
          var callbacks = new PermissionCallbacks();
          callbacks.PermissionGranted += _ => permissionRequestFinished = true;
          callbacks.PermissionDenied += _ => permissionRequestFinished = true;
          callbacks.PermissionDeniedAndDontAskAgain += _ => permissionRequestFinished = true;
          Permission.RequestUserPermission(Permission.Camera, callbacks);
          yield return new WaitUntil(() => permissionRequestFinished);
        }
#elif UNITY_IOS
        if (!Application.HasUserAuthorization(UserAuthorization.WebCam)) {
          yield return Application.RequestUserAuthorization(UserAuthorization.WebCam);
        }
#endif

#if UNITY_ANDROID
        if (!Permission.HasUserAuthorizedPermission(Permission.Camera))
        {
          Debug.LogWarning("Not permitted to use Camera");
          yield break;
        }
#elif UNITY_IOS
        if (!Application.HasUserAuthorization(UserAuthorization.WebCam)) {
          Debug.LogWarning("Not permitted to use WebCam");
          yield break;
        }
#endif
        _IsPermitted = true;

        yield return new WaitForEndOfFrame();
      }
    }

    public override void SelectSource(int sourceId)
    {
      if (sourceId < 0 || sourceId >= availableSources.Length)
      {
        throw new ArgumentException($"Invalid source ID: {sourceId}");
      }

      webCamDevice = availableSources[sourceId];
    }

    public override IEnumerator Play()
    {
      // Face landmarking and segmentation request this same source concurrently.
      // Only one caller owns startup; the other waits for actual camera frames.
      if (_isStarting)
      {
        yield return new WaitUntil(() => !_isStarting);
        if (!isPrepared) throw new InvalidOperationException("Shared camera startup failed; see the first camera error.");
        yield break;
      }
      if (isPrepared && isPlaying) yield break;

      _isStarting = true;
      try
      {
        yield return Initialize();
        if (!_IsPermitted)
          throw new InvalidOperationException("Not permitted to access cameras");
        InitializeWebCamTexture();
        Debug.Log($"HearSay camera: starting {sourceName}");
        webCamTexture.Play();
        yield return WaitForWebCamTexture();
      }
      finally
      {
        _isStarting = false;
      }
    }

    public override IEnumerator Resume()
    {
      if (!isPrepared)
      {
        throw new InvalidOperationException("WebCamTexture is not prepared yet");
      }
      if (!webCamTexture.isPlaying)
      {
        webCamTexture.Play();
      }
      yield return WaitForWebCamTexture();
    }

    public override void Pause()
    {
      if (isPlaying)
      {
        webCamTexture.Pause();
      }
    }

    public override void Stop()
    {
      _hasFrame = false;
      var textureToRelease = webCamTexture;
      if (textureToRelease != null)
      {
        textureToRelease.Stop();
      }
      webCamTexture = null;
      if (textureToRelease != null) UnityEngine.Object.Destroy(textureToRelease);
    }

    public override Texture GetCurrentTexture() => webCamTexture;

    private ResolutionStruct GetDefaultResolution()
    {
      var resolutions = availableResolutions;
      return resolutions == null || resolutions.Length == 0 ? new ResolutionStruct() : resolutions.OrderBy(resolution => resolution, new ResolutionStructComparer(_preferableDefaultWidth)).First();
    }

    private void InitializeWebCamTexture()
    {
      Stop();
      if (webCamDevice is WebCamDevice valueOfWebCamDevice)
      {
#if UNITY_ANDROID && !UNITY_EDITOR
        // Match the working Conversation Partner capture request. Android chooses
        // the nearest supported size; downstream code uses the actual frame size.
        webCamTexture = new WebCamTexture(valueOfWebCamDevice.name, 1280, 720, 30);
#else
        webCamTexture = new WebCamTexture(valueOfWebCamDevice.name, resolution.width, resolution.height, (int)resolution.frameRate);
#endif
        return;
      }
      throw new InvalidOperationException("Cannot initialize WebCamTexture because WebCamDevice is not selected");
    }

    private IEnumerator WaitForWebCamTexture()
    {
      float deadline = Time.realtimeSinceStartup + 15f;
      while (webCamTexture != null && Time.realtimeSinceStartup < deadline)
      {
        if (webCamTexture.width > 16 && webCamTexture.height > 16 && webCamTexture.didUpdateThisFrame)
        {
          _hasFrame = true;
          Debug.Log($"HearSay camera ready: {sourceName}, {webCamTexture.width}x{webCamTexture.height}, rotation={webCamTexture.videoRotationAngle}");
          yield break;
        }
        yield return null;
      }
      Stop();
      throw new TimeoutException($"HearSay camera {sourceName} did not deliver frames within 15 seconds. Check camera access and other camera users.");
    }

    private class ResolutionStructComparer : IComparer<ResolutionStruct>
    {
      private readonly int _preferableDefaultWidth;

      public ResolutionStructComparer(int preferableDefaultWidth)
      {
        _preferableDefaultWidth = preferableDefaultWidth;
      }

      public int Compare(ResolutionStruct a, ResolutionStruct b)
      {
        var aDiff = Mathf.Abs(a.width - _preferableDefaultWidth);
        var bDiff = Mathf.Abs(b.width - _preferableDefaultWidth);
        if (aDiff != bDiff)
        {
          return aDiff - bDiff;
        }
        if (a.height != b.height)
        {
          // prefer smaller height
          return a.height - b.height;
        }
        // prefer smaller frame rate
        return (int)(a.frameRate - b.frameRate);
      }
    }
  }
}
