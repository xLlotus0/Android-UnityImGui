#include "Hooking/Input/InputEventBridge.h"

#include <cstddef>
#include <cstdint>
#include <android/log.h>
#include <android/native_window.h>
#include <android/input.h>

#include "Dobby/dobby.h"
#include "Foundation/ElfScanner/ElfScannerManager.h"
#include "Foundation/Logger.h"

constexpr const char* sdk33_android_InputConsumer_consume = "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventEPiS7_Pb";
constexpr const char* sdk35_android_InputConsumer_consume = "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE";

static Hooking::InputEventBridge::Callback g_InputEventCallback;
int32_t (*Orig_consume)(void*, void*, bool, long, uint32_t*, AInputEvent**, int*, int*, bool*);
int32_t Fake_consume(void* thiz, void* factory, bool consumeBatches, long frameTime, uint32_t* outSeq, AInputEvent** outEvent, int* a6, int* a7, bool* a8) {
    if (outEvent) {
        *outEvent = nullptr;
    }

    int32_t result = Orig_consume(thiz, factory, consumeBatches, frameTime, outSeq, outEvent, a6, a7, a8);

    constexpr int32_t STATUS_OK = 0;
    if (result == STATUS_OK && g_InputEventCallback && outEvent && *outEvent) {
        g_InputEventCallback(thiz, *outEvent);
    }
    return result;
}

namespace Hooking::InputEventBridge
{
    void Install(Callback callback)
    {
        SetCallback(std::move(callback));

        int device_api_level = android_get_device_api_level();
        LOGI("[InputEventBridge] android_get_device_api_level: %d", device_api_level);

        const char* func_name = device_api_level <= 33 ? sdk33_android_InputConsumer_consume : sdk35_android_InputConsumer_consume;

        auto symbol = Elf.input().findSymbol(func_name);
        if (!symbol) {
            LOGE("[InputEventBridge] failed to find symbol: %s", func_name);
            return;
        }
        LOGI("[InputEventBridge] android::InputConsumer::consume: %p", (void*)symbol);
        DobbyHook((void*)symbol, (void*)Fake_consume, (void**)&Orig_consume);
    }

    void SetCallback(Callback callback)
    {
        g_InputEventCallback = std::move(callback);
    }
} // namespace Hooking::InputEventBridge
