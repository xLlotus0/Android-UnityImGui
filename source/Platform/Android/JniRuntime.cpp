#include "JniRuntime.h"

#include "Foundation/ElfScanner/ElfScannerManager.h"
#include "Foundation/Logger.h"

namespace AndroidPlatform
{

JavaVM* GetJavaVM()
{
    static JavaVM* s_vm = nullptr;
    if (s_vm) return s_vm;

    auto addr = Elf.art().findSymbol("JNI_GetCreatedJavaVMs");
    if (!addr)
    {
        LOGE("[JniRuntime] GetJavaVM: findSymbol(JNI_GetCreatedJavaVMs) failed");
        return nullptr;
    }

    using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM**, jsize, jsize*);
    auto fn = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(addr);

    JavaVM* vm = nullptr;
    jsize count = 0;
    if (fn(&vm, 1, &count) == JNI_OK && count > 0)
    {
        s_vm = vm;
        LOGI("[JniRuntime] GetJavaVM: got VM=%p", s_vm);
        return s_vm;
    }

    LOGE("[JniRuntime] GetJavaVM: JNI_GetCreatedJavaVMs failed, count=%d", (int)count);
    return nullptr;
}

JNIEnv* GetJavaEnv()
{
    JavaVM* vm = GetJavaVM();
    if (!vm)
    {
        LOGE("[JniRuntime] GetJavaEnv: JavaVM is null");
        return nullptr;
    }
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    LOGW("[JniRuntime] GetJavaEnv: GetEnv failed (thread not attached?)");
    return nullptr;
}

}
