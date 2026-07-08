#include "Runtime/FrameClock/UnityFrameHook.h"

#include <atomic>
#include <cstdint>
#include <utility>

#include "Dobby/dobby.h"
#include "Foundation/ElfScanner/ElfScannerManager.h"
#include "Foundation/Logger.h"

namespace Runtime::UnityFrameHook
{

namespace
{

constexpr uintptr_t kInvokeOnBeforeRenderOffset = 0x3712d84;

using InvokeOnBeforeRenderFn = void (*)(void* methodInfo);

std::atomic<bool> g_Installed{false};
TickCallback g_Callback;
InvokeOnBeforeRenderFn g_OriginalInvokeOnBeforeRender = nullptr;

void HookedInvokeOnBeforeRender(void* methodInfo)
{
    if (g_OriginalInvokeOnBeforeRender)
        g_OriginalInvokeOnBeforeRender(methodInfo);

    if (g_Callback)
        g_Callback(0.0f);
}

} // namespace

bool Install(TickCallback callback)
{
    if (g_Installed.exchange(true))
        return true;

    g_Callback = std::move(callback);

    ElfScanner& il2cpp = Elf.il2cpp();
    if (!il2cpp.isValid())
    {
        LOGE("[UnityFrameHook] libil2cpp.so is not scanned");
        g_Installed.store(false);
        return false;
    }

    void* target = reinterpret_cast<void*>(il2cpp.base() + kInvokeOnBeforeRenderOffset);
    int result = DobbyHook(
        target,
        reinterpret_cast<void*>(HookedInvokeOnBeforeRender),
        reinterpret_cast<void**>(&g_OriginalInvokeOnBeforeRender));

    if (result != 0)
    {
        LOGE("[UnityFrameHook] DobbyHook failed: target=%p result=%d", target, result);
        g_OriginalInvokeOnBeforeRender = nullptr;
        g_Callback = nullptr;
        g_Installed.store(false);
        return false;
    }

    LOGI("[UnityFrameHook] hooked UnityEngine.Application.InvokeOnBeforeRender at %p (libil2cpp base=%p offset=0x%lx)",
         target, reinterpret_cast<void*>(il2cpp.base()), static_cast<unsigned long>(kInvokeOnBeforeRenderOffset));
    return true;
}

} // namespace Runtime::UnityFrameHook
