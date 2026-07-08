#include "Runtime/ApplicationRuntime.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

#include <android/input.h>

#include "Foundation/ElfScanner/ElfScannerManager.h"
#include "Foundation/HookUtils.h"
#include "Foundation/KittyEx.h"
#include "Foundation/Logger.h"
#include "Hooking/Input/InputEventBridge.h"
#include "Integration/AndroidIme/ImGuiAndroidIme.h"
#include "Platform/Android/AndroidPlatform.h"
#include "Product/Overlay/OverlayConfig.h"
#include "Product/Overlay/OverlayRenderer.h"
#include "Render/ImGui/ImGuiHost.h"
#include "Runtime/FrameClock/UnityFrameHook.h"

#include "imgui/backends/imgui_impl_android.h"

namespace Runtime
{

namespace
{

std::atomic<bool> g_OverlayInitRequested{false};
std::mutex g_InputConsumerMutex;
void* g_ActiveInputConsumer = nullptr;

bool ShouldForwardInputEvent(void* consumer, AInputEvent* event)
{
    if (!event)
        return false;

    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return true;

    int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    std::lock_guard<std::mutex> lock(g_InputConsumerMutex);
    switch (action)
    {
    case AMOTION_EVENT_ACTION_DOWN:
        if (g_ActiveInputConsumer && g_ActiveInputConsumer != consumer)
            return false;
        g_ActiveInputConsumer = consumer;
        return true;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
    {
        bool accept = (g_ActiveInputConsumer == nullptr || g_ActiveInputConsumer == consumer);
        if (accept)
            g_ActiveInputConsumer = nullptr;
        return accept;
    }
    default:
        if (g_ActiveInputConsumer && g_ActiveInputConsumer != consumer)
            return false;
        return true;
    }
}

void OnUnityFrame(float /*deltaTime*/)
{
    if (!g_OverlayInitRequested.load(std::memory_order_acquire))
    {
        if (!AndroidPlatform::GetActivity())
            return;

        bool expected = false;
        if (g_OverlayInitRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            OverlayUi::RuntimeConfig& config = OverlayUi::GetConfig();
            ImGuiHost::Init({
                .secureCapture = config.secureCapture,
                .render = OverlayUi::Render,
            });
            LOGI("[ApplicationRuntime] ImGui init requested on Unity frame thread");
        }
    }

    ImGuiHost::Tick();
}

bool ScanRequiredLibraries()
{
    for (int i = 0; i < 200; ++i)
    {
        if (Elf.Scan({
                "libinput.so",
                "libart.so",
                "libil2cpp.so",
            }))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void HandleInputEvent(void* consumer, AInputEvent* event)
{
    if (!event)
        return;

    if (ImGuiHost::IsInitialized() && ShouldForwardInputEvent(consumer, event))
        ImGui_ImplAndroid_HandleInputEvent(event);

    int32_t eventType = AInputEvent_getType(event);
    if (eventType != AINPUT_EVENT_TYPE_KEY)
        return;

    int32_t eventKeyCode = AKeyEvent_getKeyCode(event);
    int32_t eventAction = AKeyEvent_getAction(event);
    if (eventKeyCode == AKEYCODE_DEL && eventAction == AKEY_EVENT_ACTION_DOWN)
        ImGuiAndroidIme::DeleteBackward();
    else if (eventKeyCode == AKEYCODE_BACK && eventAction == AKEY_EVENT_ACTION_DOWN)
        ImGuiAndroidIme::HideKeyboard();
    else if (eventKeyCode == AKEYCODE_VOLUME_DOWN && eventAction == AKEY_EVENT_ACTION_DOWN)
        LOGI("keycode: AKEYCODE_VOLUME_DOWN, action: AKEY_EVENT_ACTION_DOWN");
    else if (eventKeyCode == AKEYCODE_VOLUME_UP && eventAction == AKEY_EVENT_ACTION_DOWN)
        LOGI("keycode: AKEYCODE_VOLUME_UP, action: AKEY_EVENT_ACTION_DOWN");
}

void RuntimeMain()
{
    KT::Init();

    if (!ScanRequiredLibraries())
    {
        LOGE("[ApplicationRuntime] failed to scan necessary libraries");
        MAKE_CRASH();
    }

    GetLogFile("Debug")->Append("Hello\n");

    Hooking::InputEventBridge::Install(HandleInputEvent);

    if (!UnityFrameHook::Install(OnUnityFrame))
    {
        LOGE("[ApplicationRuntime] failed to install Unity frame hook");
        MAKE_CRASH();
    }
}

} // namespace

void Launch()
{
    std::thread(RuntimeMain).detach();
}

void Shutdown()
{
    ImGuiAndroidIme::HideKeyboard();
    ImGuiHost::Clean();
}

} // namespace Runtime
