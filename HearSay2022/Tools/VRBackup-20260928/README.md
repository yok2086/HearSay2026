# HearSay VR experiment — 28 September 2026

Status: experimental, not phone-verified. Runtime C# compilation passed. Unity's
batch build attempt aborted during macOS application registration with
`attempt to write a readonly database`; no verified APK was produced.

Original project: Unity 2023.2.22f1. No Unity version change.

## Backed-up shared files

- Packages/manifest.json: added Cardboard v1.24.0 Git dependency.
- Packages/packages-lock.json: Unity may resolve this after the manifest change.
- ProjectSettings/ProjectSettings.asset: androidUseSwappy 1 -> 0;
  AndroidMinSdkVersion 24 -> 26. No other intended setting changes.
- Scripts/HearSayDemoVisuals.cs: snapshot of the existing script. The working
  version adds VR-only saved server URL selection and skips flat IMGUI controls
  during VR. Normal behavior is unchanged when the VR adapter is absent.

New files are contained in Assets/HearSayVR (and its Unity-generated .meta).
The original HearSay scenes were not edited. Follow-up scene-loading fix:
EditorBuildSettings.asset is now backed up in ProjectSettings here. Both VR scenes
were appended and enabled so SceneManager.LoadScene also works in Editor Play mode.
The existing startup scene and other scene entries are unchanged. For undo, remove
only these two appended entries, preserving any later build-list changes.

## How to test after Unity opens normally

Use HearSay > VR > Open VR setup. Then HearSay > VR > Build separate VR test APK.
This build includes the two VR scenes and normal home/demo as exit destinations,
without changing the usual build-scene list. It uses app ID suffix .vrtest so it
does not overwrite the working APK. The build menu restores app ID/name afterward.
On the new APK enter the server URL (its app preferences are separate), scan the
headset QR code before starting the camera, then start VR. The gear returns to
camera-free setup and close returns to normal HearSay. Desktop split-screen is
only a layout preview, not a headset calibration test.

The copied demo runs the existing camera, segmentation, face and Whisper code.
The adapter composites canvas UI into one texture then displays it to both eyes.
It is a flat monoscopic passthrough panel, not stereo depth or world-anchored AR.
IMGUI diagnostic/home buttons are replaced by Cardboard's controls. This is not
a complete VR conversion of the conversation-partner role or start screen.
Keep rear camera uncovered, test seated, and do not walk using this prototype.

## Verification still needed

- Unity project reimport/compile including editor scripts and shader.
- Android Gradle build with Cardboard dependencies alongside MediaPipe.
- Camera rotation, portrait/landscape aspect, face contour/bubble alignment in headset.
- Enter/exit VR; permissions; normal HearSay regression build.
- Frame time/latency: composition adds one full-screen render per frame.

## Undo requested by user

Compare current files to these snapshots before restoring, preserving later edits.
Remove only the added Cardboard dependency, restore the two changed PlayerSettings
values, and remove the two VR-only additions in HearSayDemoVisuals. Remove or move
Assets/HearSayVR and its .meta outside Assets. Let Unity resolve the package lock
again, or restore the backed-up lock if no later package changes exist. Do not use
git reset or replace unrelated project settings from an older Git commit.

The Gradle callback adds Cardboard Java dependencies only to generated build files.
They are shared by all Android builds while this package is installed. No custom
manifest or persistent Gradle template was changed.
