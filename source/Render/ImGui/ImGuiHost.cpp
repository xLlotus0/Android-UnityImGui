#include "ImGuiHost.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include "Platform/Android/AndroidPlatform.h"
#include "Render/Backend/VulkanBackend.h"
#include "ImGuiSetup.h"
#include "Foundation/Logger.h"
#include "imgui/backends/imgui_impl_android.h"

namespace ImGuiHost
{

namespace
{

constexpr jint kOverlayBaseFlags =
    512 | 8 | 16 | 32 | 1024 | 256 | 0x01000000 | 1073741824;
constexpr jint kPixelFormatTranslucent = -3;

std::mutex g_Mutex;
InitOptions g_Options;
std::unique_ptr<VulkanBackend> g_Backend;
ANativeWindow* g_OverlayWindow = nullptr;
jobject g_OverlaySurfaceView = nullptr;
int g_OverlayWidth = 0;
int g_OverlayHeight = 0;
bool g_Started = false;
bool g_ContextReady = false;
std::atomic<bool> g_Initialized{false};
std::atomic<bool> g_OverlayThreadStarted{false};
std::atomic<bool> g_CaptureHiding{false};

bool ClearException(JNIEnv* env)
{
    if (!env || !env->ExceptionCheck())
        return false;
    env->ExceptionClear();
    return true;
}

JNIEnv* GetEnv(bool& needDetach)
{
    needDetach = false;
    JavaVM* vm = AndroidPlatform::GetJavaVM();
    if (!vm)
        return nullptr;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        return env;

    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return nullptr;

    needDetach = true;
    return env;
}

void DetachEnv(bool needDetach)
{
    if (!needDetach)
        return;

    JavaVM* vm = AndroidPlatform::GetJavaVM();
    if (vm)
        vm->DetachCurrentThread();
}

void PrepareJavaLooper(JNIEnv* env)
{
    if (!env)
        return;

    jclass looperClass = env->FindClass("android/os/Looper");
    if (!looperClass || ClearException(env))
        return;

    jmethodID myLooper = env->GetStaticMethodID(looperClass, "myLooper", "()Landroid/os/Looper;");
    jobject currentLooper = myLooper ? env->CallStaticObjectMethod(looperClass, myLooper) : nullptr;
    if (ClearException(env))
        currentLooper = nullptr;

    if (!currentLooper)
    {
        jmethodID prepare = env->GetStaticMethodID(looperClass, "prepare", "()V");
        if (prepare && !ClearException(env))
            env->CallStaticVoidMethod(looperClass, prepare);
        ClearException(env);
    }
    else
    {
        env->DeleteLocalRef(currentLooper);
    }

    env->DeleteLocalRef(looperClass);
}

void LoopJavaLooper(JNIEnv* env)
{
    if (!env)
        return;

    jclass looperClass = env->FindClass("android/os/Looper");
    if (!looperClass || ClearException(env))
        return;

    jmethodID loop = env->GetStaticMethodID(looperClass, "loop", "()V");
    if (loop && !ClearException(env))
        env->CallStaticVoidMethod(looperClass, loop);
    ClearException(env);
    env->DeleteLocalRef(looperClass);
}

jobject GetSurfaceControlFromSurfaceView(JNIEnv* env, jobject surfaceView)
{
    jclass surfaceViewClass = env->GetObjectClass(surfaceView);
    if (!surfaceViewClass || ClearException(env))
        return nullptr;

    jobject surfaceControl = nullptr;
    jmethodID getSurfaceControl = env->GetMethodID(surfaceViewClass, "getSurfaceControl", "()Landroid/view/SurfaceControl;");
    if (getSurfaceControl && !ClearException(env))
        surfaceControl = env->CallObjectMethod(surfaceView, getSurfaceControl);
    ClearException(env);

    if (!surfaceControl)
    {
        jfieldID surfaceControlField = env->GetFieldID(surfaceViewClass, "mSurfaceControl", "Landroid/view/SurfaceControl;");
        if (surfaceControlField && !ClearException(env))
            surfaceControl = env->GetObjectField(surfaceView, surfaceControlField);
        ClearException(env);
    }

    env->DeleteLocalRef(surfaceViewClass);
    return surfaceControl;
}

void CloseTransaction(JNIEnv* env, jobject transaction)
{
    if (!env || !transaction)
        return;

    jclass transactionClass = env->GetObjectClass(transaction);
    jmethodID close = transactionClass ? env->GetMethodID(transactionClass, "close", "()V") : nullptr;
    if (close && !ClearException(env))
        env->CallVoidMethod(transaction, close);
    ClearException(env);
    if (transactionClass)
        env->DeleteLocalRef(transactionClass);
}

bool TryApplySkipScreenshot(JNIEnv* env, jobject surfaceView, bool enabled)
{
    jobject surfaceControl = GetSurfaceControlFromSurfaceView(env, surfaceView);
    if (!surfaceControl || ClearException(env))
        return false;

    jclass transactionClass = env->FindClass("android/view/SurfaceControl$Transaction");
    if (!transactionClass || ClearException(env))
    {
        env->DeleteLocalRef(surfaceControl);
        return false;
    }

    jmethodID ctor = env->GetMethodID(transactionClass, "<init>", "()V");
    jobject transaction = (ctor && !ClearException(env)) ? env->NewObject(transactionClass, ctor) : nullptr;
    if (!transaction || ClearException(env))
    {
        env->DeleteLocalRef(transactionClass);
        env->DeleteLocalRef(surfaceControl);
        return false;
    }

    bool applied = false;
    jmethodID setSkipScreenshot = env->GetMethodID(transactionClass, "setSkipScreenshot",
        "(Landroid/view/SurfaceControl;Z)Landroid/view/SurfaceControl$Transaction;");
    if (setSkipScreenshot && !ClearException(env))
    {
        jobject ret = env->CallObjectMethod(transaction, setSkipScreenshot, surfaceControl, enabled ? JNI_TRUE : JNI_FALSE);
        ClearException(env);
        if (ret)
            env->DeleteLocalRef(ret);
        applied = true;
    }
    else
    {
        ClearException(env);
        setSkipScreenshot = env->GetMethodID(transactionClass, "setSkipScreenshot",
            "(Landroid/view/SurfaceControl;Z)V");
        if (setSkipScreenshot && !ClearException(env))
        {
            env->CallVoidMethod(transaction, setSkipScreenshot, surfaceControl, enabled ? JNI_TRUE : JNI_FALSE);
            ClearException(env);
            applied = true;
        }
    }

    if (applied)
    {
        jmethodID apply = env->GetMethodID(transactionClass, "apply", "()V");
        if (apply && !ClearException(env))
            env->CallVoidMethod(transaction, apply);
        ClearException(env);
    }

    CloseTransaction(env, transaction);
    env->DeleteLocalRef(transaction);
    env->DeleteLocalRef(transactionClass);
    env->DeleteLocalRef(surfaceControl);
    return applied;
}

bool TryApplyScreenshotMetadata(JNIEnv* env, jobject surfaceView, bool enabled)
{
    jobject surfaceControl = GetSurfaceControlFromSurfaceView(env, surfaceView);
    if (!surfaceControl || ClearException(env))
        return false;

    jclass transactionClass = env->FindClass("android/view/SurfaceControl$Transaction");
    jclass parcelClass = env->FindClass("android/os/Parcel");
    if (!transactionClass || !parcelClass || ClearException(env))
    {
        if (parcelClass) env->DeleteLocalRef(parcelClass);
        if (transactionClass) env->DeleteLocalRef(transactionClass);
        env->DeleteLocalRef(surfaceControl);
        return false;
    }

    jmethodID ctor = env->GetMethodID(transactionClass, "<init>", "()V");
    jobject transaction = (ctor && !ClearException(env)) ? env->NewObject(transactionClass, ctor) : nullptr;
    jmethodID obtain = env->GetStaticMethodID(parcelClass, "obtain", "()Landroid/os/Parcel;");
    jobject parcel = (obtain && !ClearException(env)) ? env->CallStaticObjectMethod(parcelClass, obtain) : nullptr;
    if (!transaction || !parcel || ClearException(env))
    {
        if (parcel) env->DeleteLocalRef(parcel);
        if (transaction) env->DeleteLocalRef(transaction);
        env->DeleteLocalRef(parcelClass);
        env->DeleteLocalRef(transactionClass);
        env->DeleteLocalRef(surfaceControl);
        return false;
    }

    jmethodID writeInt = env->GetMethodID(parcelClass, "writeInt", "(I)V");
    if (writeInt && !ClearException(env))
        env->CallVoidMethod(parcel, writeInt, enabled ? 441731 : 0);
    ClearException(env);

    bool applied = false;
    jmethodID setMetadata = env->GetMethodID(transactionClass, "setMetadata",
        "(Landroid/view/SurfaceControl;ILandroid/os/Parcel;)Landroid/view/SurfaceControl$Transaction;");
    if (setMetadata && !ClearException(env))
    {
        jobject ret = env->CallObjectMethod(transaction, setMetadata, surfaceControl, 2, parcel);
        ClearException(env);
        if (ret)
            env->DeleteLocalRef(ret);
        applied = true;
    }
    else
    {
        ClearException(env);
        setMetadata = env->GetMethodID(transactionClass, "setMetadata",
            "(Landroid/view/SurfaceControl;ILandroid/os/Parcel;)V");
        if (setMetadata && !ClearException(env))
        {
            env->CallVoidMethod(transaction, setMetadata, surfaceControl, 2, parcel);
            ClearException(env);
            applied = true;
        }
    }

    if (applied)
    {
        jmethodID apply = env->GetMethodID(transactionClass, "apply", "()V");
        if (apply && !ClearException(env))
            env->CallVoidMethod(transaction, apply);
        ClearException(env);
    }

    jmethodID recycle = env->GetMethodID(parcelClass, "recycle", "()V");
    if (recycle && !ClearException(env))
        env->CallVoidMethod(parcel, recycle);
    ClearException(env);

    CloseTransaction(env, transaction);
    env->DeleteLocalRef(parcel);
    env->DeleteLocalRef(transaction);
    env->DeleteLocalRef(parcelClass);
    env->DeleteLocalRef(transactionClass);
    env->DeleteLocalRef(surfaceControl);
    return applied;
}

void ApplyOverlayCaptureHidingNow(bool enabled)
{
    jobject surfaceView = g_OverlaySurfaceView;
    if (!surfaceView)
        return;

    bool needDetach = false;
    JNIEnv* env = GetEnv(needDetach);
    if (!env)
        return;

    bool appliedSkipScreenshot = TryApplySkipScreenshot(env, surfaceView, enabled);
    bool appliedMetadata = TryApplyScreenshotMetadata(env, surfaceView, enabled);
    if (appliedSkipScreenshot || appliedMetadata)
    {
        LOGI("[ImGuiHost] overlay capture hiding hint=%d", enabled ? 1 : 0);
    }

    DetachEnv(needDetach);
}

jobject CreateOverlayLayoutParams(JNIEnv* env, int width, int height)
{
    jclass lpClass = env->FindClass("android/view/WindowManager$LayoutParams");
    jmethodID lpInit = lpClass ? env->GetMethodID(lpClass, "<init>", "()V") : nullptr;
    jobject lp = (lpInit && !ClearException(env)) ? env->NewObject(lpClass, lpInit) : nullptr;
    if (!lp || ClearException(env))
        return nullptr;

    env->SetIntField(lp, env->GetFieldID(lpClass, "type", "I"), 2);
    env->SetIntField(lp, env->GetFieldID(lpClass, "flags", "I"), kOverlayBaseFlags);
    env->SetIntField(lp, env->GetFieldID(lpClass, "gravity", "I"), 3 | 48);
    env->SetIntField(lp, env->GetFieldID(lpClass, "format", "I"), kPixelFormatTranslucent);
    env->SetIntField(lp, env->GetFieldID(lpClass, "width", "I"), width);
    env->SetIntField(lp, env->GetFieldID(lpClass, "height", "I"), height);
    ClearException(env);

    env->DeleteLocalRef(lpClass);
    return lp;
}

jobject CreateAndAttachSurfaceView(JNIEnv* env, jobject activity, int width, int height)
{
    if (!env || !activity || width <= 0 || height <= 0)
        return nullptr;

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getSystemService = activityClass ? env->GetMethodID(
        activityClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
    jstring serviceName = env->NewStringUTF("window");
    jobject windowManager = (getSystemService && serviceName && !ClearException(env))
        ? env->CallObjectMethod(activity, getSystemService, serviceName) : nullptr;
    ClearException(env);
    if (serviceName)
        env->DeleteLocalRef(serviceName);
    if (!windowManager)
    {
        if (activityClass)
            env->DeleteLocalRef(activityClass);
        return nullptr;
    }

    jclass surfaceViewClass = env->FindClass("android/view/SurfaceView");
    jmethodID surfaceViewInit = surfaceViewClass
        ? env->GetMethodID(surfaceViewClass, "<init>", "(Landroid/content/Context;)V") : nullptr;
    jobject surfaceView = (surfaceViewInit && !ClearException(env))
        ? env->NewObject(surfaceViewClass, surfaceViewInit, activity) : nullptr;
    if (!surfaceView || ClearException(env))
    {
        if (surfaceViewClass)
            env->DeleteLocalRef(surfaceViewClass);
        if (activityClass)
            env->DeleteLocalRef(activityClass);
        env->DeleteLocalRef(windowManager);
        return nullptr;
    }

    jmethodID setZOrderOnTop = env->GetMethodID(surfaceViewClass, "setZOrderOnTop", "(Z)V");
    if (setZOrderOnTop && !ClearException(env))
        env->CallVoidMethod(surfaceView, setZOrderOnTop, JNI_TRUE);
    ClearException(env);

    jmethodID getHolder = env->GetMethodID(surfaceViewClass, "getHolder", "()Landroid/view/SurfaceHolder;");
    jobject surfaceHolder = (getHolder && !ClearException(env)) ? env->CallObjectMethod(surfaceView, getHolder) : nullptr;
    ClearException(env);
    if (surfaceHolder)
    {
        jclass holderClass = env->GetObjectClass(surfaceHolder);
        jmethodID setFormat = holderClass ? env->GetMethodID(holderClass, "setFormat", "(I)V") : nullptr;
        if (setFormat && !ClearException(env))
            env->CallVoidMethod(surfaceHolder, setFormat, kPixelFormatTranslucent);
        ClearException(env);
        if (holderClass)
            env->DeleteLocalRef(holderClass);
    }

    jobject layoutParams = CreateOverlayLayoutParams(env, width, height);
    jclass windowManagerClass = env->GetObjectClass(windowManager);
    jmethodID addView = windowManagerClass ? env->GetMethodID(
        windowManagerClass, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V") : nullptr;
    if (addView && layoutParams && !ClearException(env))
        env->CallVoidMethod(windowManager, addView, surfaceView, layoutParams);
    if (ClearException(env))
        surfaceView = nullptr;

    jobject result = surfaceView ? env->NewGlobalRef(surfaceView) : nullptr;

    if (windowManagerClass)
        env->DeleteLocalRef(windowManagerClass);
    if (layoutParams)
        env->DeleteLocalRef(layoutParams);
    if (surfaceHolder)
        env->DeleteLocalRef(surfaceHolder);
    if (surfaceView)
        env->DeleteLocalRef(surfaceView);
    if (surfaceViewClass)
        env->DeleteLocalRef(surfaceViewClass);
    if (activityClass)
        env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(windowManager);
    return result;
}

void CreateOverlaySurfaceViewOnCurrentThread(JNIEnv* env)
{
    if (!env)
        return;

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        if (g_OverlaySurfaceView)
            return;
    }

    jobject activity = AndroidPlatform::GetActivity();
    if (!activity)
    {
        LOGE("[ImGuiHost] no Activity available, cannot create overlay");
        return;
    }

    AndroidPlatform::DisplaySize size = AndroidPlatform::GetDisplaySize();
    if (size.width <= 0 || size.height <= 0)
    {
        LOGE("[ImGuiHost] invalid display size for overlay: %dx%d", size.width, size.height);
        return;
    }

    jobject surfaceView = nullptr;
    if (env->PushLocalFrame(32) == 0)
    {
        surfaceView = CreateAndAttachSurfaceView(env, activity, size.width, size.height);
        env->PopLocalFrame(nullptr);
    }
    ClearException(env);

    if (!surfaceView)
    {
        LOGE("[ImGuiHost] failed to create overlay SurfaceView");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_OverlaySurfaceView = surfaceView;
        g_OverlayWidth = size.width;
        g_OverlayHeight = size.height;
    }
    LOGI("[ImGuiHost] overlay SurfaceView ready %dx%d view=%p", size.width, size.height, surfaceView);
}

void StartOverlaySurfaceThread()
{
    if (g_OverlayThreadStarted.exchange(true))
        return;

    std::thread([]()
    {
        bool needDetach = false;
        JNIEnv* env = GetEnv(needDetach);
        if (!env)
        {
            g_OverlayThreadStarted.store(false);
            return;
        }

        PrepareJavaLooper(env);
        CreateOverlaySurfaceViewOnCurrentThread(env);
        LoopJavaLooper(env);
        DetachEnv(needDetach);
    }).detach();
}

ANativeWindow* AcquireOverlayWindow()
{
    jobject surfaceView = g_OverlaySurfaceView;
    if (!surfaceView)
        return nullptr;

    bool needDetach = false;
    JNIEnv* env = GetEnv(needDetach);
    if (!env)
        return nullptr;

    jclass surfaceViewClass = env->GetObjectClass(surfaceView);
    jmethodID getHolder = surfaceViewClass
        ? env->GetMethodID(surfaceViewClass, "getHolder", "()Landroid/view/SurfaceHolder;") : nullptr;
    jobject surfaceHolder = (getHolder && !ClearException(env)) ? env->CallObjectMethod(surfaceView, getHolder) : nullptr;
    ClearException(env);
    if (surfaceViewClass)
        env->DeleteLocalRef(surfaceViewClass);
    if (!surfaceHolder)
    {
        DetachEnv(needDetach);
        return nullptr;
    }

    jclass holderClass = env->GetObjectClass(surfaceHolder);
    jmethodID getSurface = holderClass ? env->GetMethodID(holderClass, "getSurface", "()Landroid/view/Surface;") : nullptr;
    jobject surface = (getSurface && !ClearException(env)) ? env->CallObjectMethod(surfaceHolder, getSurface) : nullptr;
    ClearException(env);
    if (holderClass)
        env->DeleteLocalRef(holderClass);
    env->DeleteLocalRef(surfaceHolder);
    if (!surface)
    {
        DetachEnv(needDetach);
        return nullptr;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    env->DeleteLocalRef(surface);
    DetachEnv(needDetach);
    return window;
}

void ShutdownLocked()
{
    if (g_Backend)
    {
        g_Backend->Shutdown();
        g_Backend.reset();
    }

    if (g_ContextReady)
    {
        ImGuiSetup::TeardownContext();
        g_ContextReady = false;
    }

    if (g_OverlayWindow)
    {
        ANativeWindow_release(g_OverlayWindow);
        g_OverlayWindow = nullptr;
    }

    g_Initialized.store(false);
}

void UpdateInputTransformLocked()
{
    if (g_OverlayWidth <= 0 || g_OverlayHeight <= 0)
        return;

    AndroidPlatform::DisplaySize inputSize{};
    ANativeWindow* appWindow = AndroidPlatform::GetNativeWindow();
    if (appWindow)
    {
        inputSize.width = ANativeWindow_getWidth(appWindow);
        inputSize.height = ANativeWindow_getHeight(appWindow);
    }
    if (inputSize.width <= 0 || inputSize.height <= 0)
        inputSize = AndroidPlatform::GetDisplaySize();
    if (inputSize.width <= 0 || inputSize.height <= 0)
        inputSize = {g_OverlayWidth, g_OverlayHeight};

    int transform = 0;
    if ((g_OverlayWidth < g_OverlayHeight) != (inputSize.width < inputSize.height))
        transform = 1;

    ImGui_ImplAndroid_SetInputTransform(
        transform,
        static_cast<float>(g_OverlayWidth),
        static_cast<float>(g_OverlayHeight),
        static_cast<float>(inputSize.width),
        static_cast<float>(inputSize.height));
}

bool EnsureReadyLocked()
{
    if (!g_Started)
        return false;

    if (!g_OverlaySurfaceView)
        return false;

    if (!g_OverlayWindow)
    {
        g_OverlayWindow = AcquireOverlayWindow();
        if (!g_OverlayWindow)
            return false;
    }

    if (g_Backend && !g_Backend->IsReady())
    {
        LOGI("[ImGuiHost] backend requested rebuild");
        ShutdownLocked();
        return false;
    }

    if (g_Backend && g_Backend->IsReady())
    {
        int width = ANativeWindow_getWidth(g_OverlayWindow);
        int height = ANativeWindow_getHeight(g_OverlayWindow);
        if (width <= 0 || height <= 0)
        {
            LOGI("[ImGuiHost] overlay window invalid %dx%d, rebuilding backend", width, height);
            ShutdownLocked();
            return false;
        }
        if (width > 0 && height > 0 && (width != g_OverlayWidth || height != g_OverlayHeight))
        {
            LOGI("[ImGuiHost] overlay size changed %dx%d -> %dx%d, rebuilding backend",
                 g_OverlayWidth, g_OverlayHeight, width, height);
            ShutdownLocked();
            return false;
        }
        return true;
    }

    ANativeWindow* platformWindow = AndroidPlatform::GetNativeWindow();
    ImGuiSetup::EnsureContext(platformWindow ? platformWindow : g_OverlayWindow);
    g_ContextReady = true;

    int windowWidth = ANativeWindow_getWidth(g_OverlayWindow);
    int windowHeight = ANativeWindow_getHeight(g_OverlayWindow);
    if (windowWidth > 0 && windowHeight > 0)
    {
        g_OverlayWidth = windowWidth;
        g_OverlayHeight = windowHeight;
    }
    else
    {
        AndroidPlatform::DisplaySize size = AndroidPlatform::GetDisplaySize();
        g_OverlayWidth = size.width;
        g_OverlayHeight = size.height;
    }

    g_Backend = std::make_unique<VulkanBackend>();
    if (!g_Backend->Init(g_OverlayWindow, g_OverlayWidth, g_OverlayHeight))
    {
        LOGE("[ImGuiHost] Vulkan overlay init failed");
        ShutdownLocked();
        return false;
    }

    g_Initialized.store(true);
    ApplyOverlayCaptureHidingNow(g_CaptureHiding.load());
    LOGI("[ImGuiHost] initialized Vulkan overlay %dx%d", g_OverlayWidth, g_OverlayHeight);
    return true;
}

} // namespace

bool Init(const InitOptions& opt)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Started)
    {
        LOGW("[ImGuiHost] Init called twice");
        return false;
    }

    g_Options = opt;
    g_CaptureHiding.store(opt.secureCapture);
    g_Started = true;
    StartOverlaySurfaceThread();
    return true;
}

void Clean()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Started = false;
    ShutdownLocked();
}

void Tick()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (!EnsureReadyLocked())
        return;

    UpdateInputTransformLocked();

    g_Backend->BeginFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (g_OverlayWidth > 0 && g_OverlayHeight > 0)
    {
        io.DisplaySize = ImVec2(static_cast<float>(g_OverlayWidth), static_cast<float>(g_OverlayHeight));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    }
    ImGui_ImplAndroid_ProcessQueuedInputEvents();
    ImGui::NewFrame();

    if (g_Options.render)
        g_Options.render();

    ImGui::Render();
    g_Backend->EndFrame();
}

bool IsInitialized()
{
    return g_Initialized.load();
}

ImVec2 GetDisplaySize()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_ContextReady)
        return ImGui::GetIO().DisplaySize;
    return ImVec2(static_cast<float>(g_OverlayWidth), static_cast<float>(g_OverlayHeight));
}

void SetCaptureHiding(bool enabled)
{
    g_CaptureHiding.store(enabled);
    ApplyOverlayCaptureHidingNow(enabled);
}

} // namespace ImGuiHost
