// Copyright (c) 2021 homuler
//
// Use of this source code is governed by an MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

using System.Collections;
using UnityEngine;

namespace Mediapipe.Unity.Sample
{
  public abstract class VisionTaskApiRunner<TTask> : BaseRunner where TTask : Tasks.Vision.Core.BaseVisionTaskApi
  {
    [SerializeField] protected Screen screen;

    private Coroutine _coroutine;
    protected TTask taskApi;

    public RunningMode runningMode;

    public override void Play()
    {
      if (_coroutine != null)
      {
        Stop();
      }
      base.Play();
      _coroutine = StartCoroutine(Run());
    }

    public override void Pause()
    {
      base.Pause();
      ImageSourceProvider.ImageSource.Pause();
    }

    public override void Resume()
    {
      base.Resume();
      var _ = StartCoroutine(ImageSourceProvider.ImageSource.Resume());
    }

    public override void Stop()
    {
      base.Stop();
      if (_coroutine != null)
      {
        StopCoroutine(_coroutine);
        _coroutine = null;
      }
      var closingTask = taskApi;
      taskApi = null;
      try
      {
        closingTask?.Close();
      }
      finally
      {
        ImageSourceProvider.ImageSource?.Stop();
      }
    }

    protected virtual void OnDestroy()
    {
      // Bootstrap and its source persist across scenes. Explicitly release the
      // native task/camera, including derived runners' texture-frame pools.
      Stop();
    }

    protected abstract IEnumerator Run();

    protected static void SetupAnnotationController<T>(AnnotationController<T> annotationController, ImageSource imageSource, bool expectedToBeMirrored = false) where T : HierarchicalAnnotation
    {
      annotationController.isMirrored = expectedToBeMirrored;
      annotationController.imageSize = new Vector2Int(imageSource.textureWidth, imageSource.textureHeight);
    }
  }
}
