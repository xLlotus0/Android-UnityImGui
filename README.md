# Android-UnityImGui

English | [简体中文](README_CN.md)

Android-UnityImGui is an Android native ImGui overlay project for Unity IL2CPP targets. It is modified from [DumpA1n/AndSwapChainHook](https://github.com/DumpA1n/AndSwapChainHook), with the original swap-chain-hook idea rebuilt into a Unity-frame-driven overlay runtime.

This project adds three main capabilities on top of the original direction:

- Screen recording protection hints for the overlay surface.
- Android system IME support for ImGui text input.
- ImGui initialization and rendering driven from the Unity game thread, synchronized with the game frame.

## Features

- Unity frame hook: hooks `UnityEngine.Application.InvokeOnBeforeRender` through Dobby and drives `ImGuiHost::Tick()` from the same Unity frame path.
- Vulkan ImGui overlay: creates an Android `SurfaceView` overlay and renders ImGui through a Vulkan backend.
- Input bridge: hooks `android::InputConsumer::consume` from `libinput.so` and forwards Android input events to ImGui.
- System keyboard bridge: uses a JNI/DEX bridge so ImGui text fields can bring up the Android system input method.
- Capture hiding: applies `SurfaceControl.Transaction.setSkipScreenshot` and metadata hints when available.
- Split project layout: bootstrap, runtime, platform, render, hook, product, and foundation layers are separated.

## Project Layout

```text
source/
  Bootstrap/            JNI entry and constructor/destructor wiring
  Runtime/              runtime startup, library scanning, Unity frame dispatch
  Runtime/FrameClock/   Unity frame hook based on InvokeOnBeforeRender
  Platform/Android/     Activity, JNI, display, looper, native window helpers
  Render/               ImGui host and Vulkan backend
  Product/Overlay/      overlay UI rendering and runtime config
  Hooking/              input hook and pointer hook infrastructure
  Integration/          Android IME bridge for ImGui text input
  Foundation/           logger, crash handler, ELF scanner, common helpers
external/
  Dobby/
  imgui/
  KittyMemoryEx/
```

## Unity Frame Hook

The current frame hook is implemented in:

```text
source/Runtime/FrameClock/UnityFrameHook.cpp
```

It installs a Dobby hook on `UnityEngine.Application.InvokeOnBeforeRender` and calls the runtime frame callback after the original function returns. The callback initializes ImGui once an Android `Activity` is available, then calls `ImGuiHost::Tick()` every Unity frame.

The offset is target-specific. If you change the target game, update `kInvokeOnBeforeRenderOffset` with the correct offset from that game's IL2CPP dump.

## Build

The active build system is CMake. The project is configured for Android `arm64-v8a` and C++20.

Requirements:

- Android NDK with Clang
- CMake
- `external/Dobby/arm64-v8a/libdobby.a`
- bundled `external/imgui`
- bundled `external/KittyMemoryEx`

Example:

```powershell
cmake `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=D:\MyTools\toolchina\android-ndk-r30-beta1\build\cmake\android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a `
  -DANDROID_PLATFORM=android-25 `
  -S . `
  -B cmake-build-release

cmake --build cmake-build-release -j 4
```

The output module name is `Android-UnityImGui`.

## Optional NDK Build Files

`Android.mk` and `Application.mk` are included as reference files for `ndk-build` style projects. They are not required by the current CMake workflow.

## Notes

- This project is intended for authorized research, debugging, and development environments.
- The anti-recording behavior depends on Android version, OEM behavior, and system policy. It should be treated as a best-effort overlay capture hint, not a guaranteed DRM mechanism.
- The Unity hook offset is not universal. Recalculate it for each target build.
