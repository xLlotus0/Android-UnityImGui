# Android-UnityImGui

[English](README.md) | 简体中文

Android-UnityImGui 是一个面向 Android Unity IL2CPP 目标的原生 ImGui 覆盖层项目。本项目修改自 [DumpA1n/AndSwapChainHook](https://github.com/DumpA1n/AndSwapChainHook)，在原本 SwapChain Hook 思路的基础上，重构成由 Unity 游戏帧驱动的 ImGui 运行时。

相比原项目方向，本项目主要增加了这些能力：

- 覆盖层防录屏/防截图提示。
- ImGui 输入框拉起 Android 系统输入法。
- ImGui 在 GameThread/Unity 帧线程上初始化和绘制，和游戏同帧运行。

## 功能

- Unity 帧 Hook：通过 Dobby hook `UnityEngine.Application.InvokeOnBeforeRender`，从 Unity 帧回调里驱动 `ImGuiHost::Tick()`。
- Vulkan ImGui 覆盖层：创建 Android `SurfaceView` 覆盖层，并使用 Vulkan 后端绘制 ImGui。
- 输入事件桥接：hook `libinput.so` 里的 `android::InputConsumer::consume`，把 Android 输入事件转发给 ImGui。
- 系统输入法桥接：通过 JNI/DEX 桥接，让 ImGui 文本输入框可以拉起 Android 系统键盘。
- 防录屏提示：在支持的系统上对覆盖层 `SurfaceControl` 应用 `setSkipScreenshot` 和 metadata hint。
- 大型项目式分层：入口、运行时、平台层、渲染层、Hook 层、业务 UI、基础设施分开维护。

## 目录结构

```text
source/
  Bootstrap/            JNI 入口、constructor/destructor
  Runtime/              运行时启动、库扫描、Unity 帧调度
  Runtime/FrameClock/   基于 InvokeOnBeforeRender 的 Unity 帧 Hook
  Platform/Android/     Activity、JNI、Display、Looper、NativeWindow 等平台能力
  Render/               ImGuiHost 和 Vulkan 渲染后端
  Product/Overlay/      覆盖层 UI 和运行时配置
  Hooking/              输入 Hook 和 Pointer Hook 基础设施
  Integration/          Android 系统输入法桥接
  Foundation/           日志、崩溃处理、ELF 扫描、通用工具
external/
  Dobby/
  imgui/
  KittyMemoryEx/
```

## GameThread 同帧逻辑

Unity 帧 Hook 的实现位置：

```text
source/Runtime/FrameClock/UnityFrameHook.cpp
```

当前通过 Dobby hook `UnityEngine.Application.InvokeOnBeforeRender`。Hook 回调会先调用原函数，然后进入运行时帧回调。运行时在拿到 Android `Activity` 后只初始化一次 ImGui，之后每个 Unity 帧都会调用 `ImGuiHost::Tick()` 完成 ImGui 新帧、UI 绘制和 Vulkan 提交。

注意：`kInvokeOnBeforeRenderOffset` 是目标游戏相关的偏移，不是通用固定值。换游戏或换版本后，需要根据对应 IL2CPP dump 重新填写。

## 构建

当前主要使用 CMake 构建，目标为 Android `arm64-v8a`，C++20。

依赖：

- Android NDK Clang
- CMake
- `external/Dobby/arm64-v8a/libdobby.a`
- 项目内置 `external/imgui`
- 项目内置 `external/KittyMemoryEx`

示例：

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

输出模块名为 `Android-UnityImGui`。

## Android.mk / Application.mk

工程根目录提供了 `Android.mk` 和 `Application.mk`，用于保留 `ndk-build` 风格的备用构建描述。当前工程不需要使用它们，默认仍以 CMake 为主。

## 注意事项

- 本项目仅用于授权环境下的研究、调试和开发。
- 防录屏能力依赖 Android 版本、厂商实现和系统策略，只能视为尽力提示，不应当当作绝对 DRM。
- Unity Hook 偏移必须按目标游戏版本重新确认，不能直接套用到所有 Unity 游戏。
