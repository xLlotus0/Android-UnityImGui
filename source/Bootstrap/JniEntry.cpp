#include <atomic>

#include <jni.h>

#include "Foundation/CrashHandler/CrashHandler.h"
#include "Foundation/Logger.h"
#include "Runtime/ApplicationRuntime.h"

namespace
{

std::atomic<bool> g_Bootstrapped{false};

} // namespace

extern "C" jint JNIEXPORT JNI_OnLoad(JavaVM* vm, void* key)
{
    if (key != reinterpret_cast<void*>(1337))
        return JNI_VERSION_1_6;

    LOGI("JNI_OnLoad called by injector.");
    LOGI("JavaVM: %p", vm);

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        LOGI("JavaEnv: %p", env);

    if (!g_Bootstrapped.exchange(true))
        Runtime::Launch();

    return JNI_VERSION_1_6;
}

__attribute__((constructor)) void ctor()
{
    LOGI("ctor");
    CrashHandler::Install();
}

__attribute__((destructor)) void dtor()
{
    Runtime::Shutdown();
    LOGI("dtor");
}

struct android_app;
extern "C" void android_main(struct android_app* /*state*/) {}
