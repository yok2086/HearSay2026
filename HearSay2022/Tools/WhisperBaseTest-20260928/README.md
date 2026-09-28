# Base English accuracy test

Only HearSay Demo's Whisper modelPath changed from Whisper/ggml-tiny.bin to
Whisper/ggml-base.en.bin. The original scene is backed up here. VR Test and
streaming timings were not changed. Tiny remains available for reverting.

Downloaded base.en from ggerganov/whisper.cpp on Hugging Face, revision
5359861c739e955e79d9a303bcbc70fb988958b1.
Verified SHA256: a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002.
Size: 147964211 bytes. Keeping both models increases APK size.

Rebuild/install the normal APK, test the same sentences at the same distance,
and compare accuracy and delay with Tiny. Model loading and runtime speed have
not yet been verified on the phone. A larger model may worsen latency.

Undo: change only modelPath back to Whisper/ggml-tiny.bin, preserving any later
scene edits. Do not overwrite the whole scene with the backup if it has changed.
