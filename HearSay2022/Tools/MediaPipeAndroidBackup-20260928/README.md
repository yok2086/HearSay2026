# MediaPipe Android dependency fix

Device log confirmed libmediapipe_jni.so exists but loading OpenCV fails because
libc++_shared.so is missing. The existing generated APK contains MediaPipe and
OpenCV, but not that C++ runtime. This is not a camera permission error.

Added Assets/Plugins/Android/arm64-v8a/libc++_shared.so from the installed Unity
2023.2.22f1 Android NDK sources/cxx-stl/llvm-libc++/libs/arm64-v8a directory, with
an Android-only ARM64 PluginImporter meta. No existing plugin or scene was changed.
Rebuild/reinstall required. Verify the APK contains lib/arm64-v8a/libc++_shared.so
and retest Live Captions on the device. Do not claim device success before testing.

To undo only this fix, remove the added libc++_shared.so and its meta (and their
folder/meta only if empty). The earlier camera startup and VR changes are separate.
