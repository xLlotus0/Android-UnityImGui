#pragma once

#include <concepts>
#include <mutex>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>

#include "IPointerHook.h"
#include <android/log.h>

#ifndef kANDROID_LOG_TAG
#define kANDROID_LOG_TAG "PtrHook"
#endif

#define PH_LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO,  kANDROID_LOG_TAG, __VA_ARGS__))
#define PH_LOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, kANDROID_LOG_TAG, __VA_ARGS__))

// Thread-safe via a single internal mutex held across map lookup + hook
// state transition. No explicit destroy verb — Restore toggles off but
// keeps the instance for trampoline reuse; the destructor walks the map
// and restores every slot at process exit / dlclose.

class PointerHookManager
{
    struct StringHash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }
        size_t operator()(const std::string& s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
        size_t operator()(const char* s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

public:
    PointerHookManager(const PointerHookManager&) = delete;
    PointerHookManager& operator=(const PointerHookManager&) = delete;

    static PointerHookManager& GetInstance()
    {
        static PointerHookManager gInstance;
        return gInstance;
    }

    template<class T, class... Args>
        requires std::derived_from<T, IPointerHook>
    void Install(Args&&... args)
    {
        InstallNamed<T>(typeid(T).name(), std::forward<Args>(args)...);
    }

    template<class T, class... Args>
        requires std::derived_from<T, IPointerHook>
    void InstallNamed(std::string name, Args&&... args)
    {
        if (!IPointerHook::PassedSelfTest()) {
            PH_LOGE("[PointerHookManager] Refusing Install(%s): SelfTest did not pass", name.c_str());
            return;
        }

        std::lock_guard<std::mutex> guard(m_mtx_);

        if (auto it = m_hooks.find(name); it != m_hooks.end()) {
            PH_LOGI("[PointerHookManager] Install (re-install): %s", it->second->GetName().c_str());
            it->second->Install();
            return;
        }

        auto hook = std::make_unique<T>(std::forward<Args>(args)...);
        hook->Resolve();
        hook->Install();

        PH_LOGI("[PointerHookManager] Install: %s (key=%s)", hook->GetName().c_str(), name.c_str());
        m_hooks.emplace(std::move(name), std::move(hook));
    }

    template<class T>
        requires std::derived_from<T, IPointerHook>
    void Restore()
    {
        RestoreNamed(typeid(T).name());
    }

    void RestoreNamed(std::string_view name)
    {
        std::lock_guard<std::mutex> guard(m_mtx_);

        auto it = m_hooks.find(name);
        if (it == m_hooks.end()) {
            PH_LOGE("[PointerHookManager] Restore: key '%.*s' not found",
                 (int)name.size(), name.data());
            return;
        }
        PH_LOGI("[PointerHookManager] Restore: %s", it->second->GetName().c_str());
        it->second->Restore();
    }

protected:
    PointerHookManager() { IPointerHook::SelfTest(); }
    ~PointerHookManager() = default;

private:
    std::mutex m_mtx_;
    std::unordered_map<std::string, std::unique_ptr<IPointerHook>,
                       StringHash, std::equal_to<>> m_hooks;
};
