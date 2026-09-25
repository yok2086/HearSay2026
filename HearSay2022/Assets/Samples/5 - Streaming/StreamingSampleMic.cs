using UnityEngine;
using UnityEngine.UI;
using System.Collections;
using Whisper.Utils;

#if UNITY_ANDROID && !UNITY_EDITOR
using UnityEngine.Android;
#endif

namespace Whisper.Samples
{
    /// <summary>
    /// Stream transcription from microphone input.
    /// </summary>
    public class StreamingSampleMic : MonoBehaviour
    {
        public WhisperManager whisper;
        public MicrophoneRecord microphoneRecord;

        [Header("UI")]
        public Button button;
        public Text buttonText;
        public Text text;
        public ScrollRect scroll;
        private WhisperStream _stream;

        private void Start()
        {
            StartCoroutine(Initialize());
        }

        private IEnumerator Initialize()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            if (!Permission.HasUserAuthorizedPermission(Permission.Microphone))
            {
                text.text = "Microphone permission needed…";
                var requestFinished = false;
                var callbacks = new PermissionCallbacks();
                callbacks.PermissionGranted += _ => requestFinished = true;
                callbacks.PermissionDenied += _ => requestFinished = true;
                callbacks.PermissionDeniedAndDontAskAgain += _ => requestFinished = true;
                Permission.RequestUserPermission(Permission.Microphone, callbacks);
                yield return new WaitUntil(() => requestFinished);
            }

            if (!Permission.HasUserAuthorizedPermission(Permission.Microphone))
            {
                text.text = "Microphone permission denied.";
                Debug.LogError("Whisper streaming: Android microphone permission was denied.");
                yield break;
            }
#endif

            var streamTask = whisper.CreateStream(microphoneRecord);
            yield return new WaitUntil(() => streamTask.IsCompleted);

            if (streamTask.IsFaulted)
            {
                text.text = "Could not start Whisper. Check Console.";
                Debug.LogException(streamTask.Exception);
                yield break;
            }

            _stream = streamTask.Result;
            _stream.OnResultUpdated += OnResult;
            _stream.OnSegmentUpdated += OnSegmentUpdated;
            _stream.OnSegmentFinished += OnSegmentFinished;
            _stream.OnStreamFinished += OnFinished;

            microphoneRecord.OnRecordStop += OnRecordStop;
            button.onClick.AddListener(OnButtonPressed);
        }

        private void OnButtonPressed()
        {
            if (!microphoneRecord.IsRecording)
            {
                _stream.StartStream();
                microphoneRecord.StartRecord();
            }
            else
                microphoneRecord.StopRecord();

            buttonText.text = microphoneRecord.IsRecording ? "Stop" : "Record";
        }

        private void OnRecordStop(AudioChunk recordedAudio)
        {
            float max = 0f;

            foreach (float sample in recordedAudio.Data)
            {
                max = Mathf.Max(max, Mathf.Abs(sample));
            }

            Debug.Log($"MIC — samples: {recordedAudio.Data.Length}, max volume: {max}");

            buttonText.text = "Record";
        }

        private void OnResult(string result)
        {
            text.text = result;
            UiUtils.ScrollDown(scroll);
        }

        private void OnSegmentUpdated(WhisperResult segment)
        {
            print($"Segment updated: {segment.Result}");
        }

        private void OnSegmentFinished(WhisperResult segment)
        {
            print($"Segment finished: {segment.Result}");
        }

        private void OnFinished(string finalResult)
        {
            print("Stream finished!");
        }
    }
}
