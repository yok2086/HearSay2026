package com.hearsay.speech;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import com.unity3d.player.UnityPlayer;
import java.util.ArrayList;

public final class HearSaySpeechBridge implements RecognitionListener {
    private final String objectName;
    private SpeechRecognizer recognizer;
    private Intent listenIntent;
    public HearSaySpeechBridge(String objectName) { this.objectName = objectName; }
    public void start(boolean preferOnDevice) {
        Context c = UnityPlayer.currentActivity;
        if (preferOnDevice && android.os.Build.VERSION.SDK_INT >= 31 && SpeechRecognizer.isOnDeviceRecognitionAvailable(c)) {
            recognizer = SpeechRecognizer.createOnDeviceSpeechRecognizer(c);
        } else {
            // This is the same configured system recognition path used by
            // Android voice typing, while results are sent back to Unity.
            recognizer = SpeechRecognizer.createSpeechRecognizer(c);
        }
        recognizer.setRecognitionListener(this);
        listenIntent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        listenIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        listenIntent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
        listenIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE, "en-US");
        recognizer.startListening(listenIntent);
    }
    public void stop() { if (recognizer != null) { recognizer.stopListening(); recognizer.destroy(); recognizer = null; } }
    private void send(String s) { UnityPlayer.UnitySendMessage(objectName, "ReceiveAndroidTranscript", s == null ? "" : s); }
    public void onResults(Bundle b) { send(first(b)); restart(); }
    public void onPartialResults(Bundle b) { send(first(b)); }
    private String first(Bundle b) { ArrayList<String> r = b.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION); return r == null || r.isEmpty() ? "" : r.get(0); }
    public void onError(int e) { UnityPlayer.UnitySendMessage(objectName, "ReceiveAndroidSpeechError", errorName(e)); restart(); }
    private void restart() { if (recognizer != null && listenIntent != null) recognizer.startListening(listenIntent); }
    private String errorName(int e) {
        switch (e) {
            case SpeechRecognizer.ERROR_AUDIO: return "audio input unavailable";
            case SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS: return "microphone permission denied";
            case SpeechRecognizer.ERROR_NETWORK: return "network error";
            case SpeechRecognizer.ERROR_NETWORK_TIMEOUT: return "network timeout";
            case SpeechRecognizer.ERROR_NO_MATCH: return "no speech heard";
            case SpeechRecognizer.ERROR_SPEECH_TIMEOUT: return "blank audio / inaudible";
            default: return "recognizer error " + e;
        }
    }
    public void onReadyForSpeech(Bundle b) {} public void onBeginningOfSpeech() {} public void onRmsChanged(float r) {} public void onBufferReceived(byte[] b) {} public void onEndOfSpeech() {} public void onEvent(int t, Bundle b) {}
}
