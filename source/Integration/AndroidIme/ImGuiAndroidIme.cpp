#include "ImGuiAndroidIme.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include <jni.h>
#include <sys/stat.h>

#include "Foundation/Logger.h"
#include "Platform/Android/AndroidPlatform.h"
#include "imgui_internal.h"

namespace ImGuiAndroidIme
{

namespace
{

#include "ImeBridgeDex.inc"

constexpr double kPollIntervalSeconds = 0.10;
constexpr jint kInputTypeText = 0x00000001;
constexpr jint kInputTypeNoSuggestions = 0x00080000;
constexpr jint kImeActionDone = 0x00000006;
constexpr jint kImeFlagNoExtractUi = 0x10000000;
constexpr jint kShowForced = 2;

enum class UiTaskType
{
    ShowKeyboard,
    PollText,
    SetText,
};

struct UiTask
{
    UiTaskType type;
    std::string text;
    bool syncText = false;
};

std::mutex g_BridgeMutex;
jclass g_RunnableClass = nullptr;
jmethodID g_RunnableCtor = nullptr;
jclass g_TextWatcherClass = nullptr;
jmethodID g_TextWatcherCtor = nullptr;
jclass g_EditTextClass = nullptr;
jmethodID g_EditTextCtor = nullptr;
jobject g_TextWatcher = nullptr;
bool g_BridgeFailed = false;

jobject g_EditText = nullptr;
std::atomic<bool> g_ShowTaskPending{false};
std::atomic<bool> g_PollTaskPending{false};

std::mutex g_TextMutex;
std::string g_SharedText;

ImGuiID g_ActiveWidget = 0;
double g_NextPollTime = 0.0;

void NativeRun(JNIEnv* env, jclass, jlong taskPtr);
void NativeTextChanged(JNIEnv* env, jclass, jstring text);
void NativeDeleteBackward(JNIEnv* env, jclass, jint count);

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

bool ClearException(JNIEnv* env)
{
    if (!env || !env->ExceptionCheck())
        return false;
    env->ExceptionClear();
    return true;
}

void UpdateSharedText(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_TextMutex);
    g_SharedText = text;
}

std::string ReadSharedText()
{
    std::lock_guard<std::mutex> lock(g_TextMutex);
    return g_SharedText;
}

bool CopyTextToBuffer(const std::string& text, char* buffer, size_t bufferSize)
{
    if (!buffer || bufferSize == 0)
        return false;

    if (std::strncmp(buffer, text.c_str(), bufferSize) == 0)
        return false;

    size_t copyLen = std::min(bufferSize - 1, text.size());
    std::memcpy(buffer, text.data(), copyLen);
    buffer[copyLen] = '\0';
    return true;
}

void ReloadActiveInputState(ImGuiID id)
{
    ImGuiInputTextState* state = ImGui::GetInputTextState(id);
    if (!state)
        return;

    state->ReloadUserBufAndMoveToEnd();
    state->Scroll.x = 0.0f;
    state->CursorFollow = true;
    state->CursorAnimReset();
}

void EraseLastUtf8Codepoint(std::string& text)
{
    if (text.empty())
        return;

    size_t pos = text.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80)
        --pos;
    text.erase(pos);
}

std::string JavaStringToStdString(JNIEnv* env, jstring value)
{
    if (!env || !value)
        return {};

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        return {};

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

std::string GetFileAbsolutePath(JNIEnv* env, jobject file)
{
    if (!env || !file)
        return {};

    jclass fileClass = env->GetObjectClass(file);
    jmethodID getAbsolutePath = fileClass ? env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;") : nullptr;
    jstring path = (getAbsolutePath && !ClearException(env))
        ? static_cast<jstring>(env->CallObjectMethod(file, getAbsolutePath)) : nullptr;
    ClearException(env);

    std::string result = JavaStringToStdString(env, path);
    if (path)
        env->DeleteLocalRef(path);
    if (fileClass)
        env->DeleteLocalRef(fileClass);
    return result;
}

std::string GetCacheDirPath(JNIEnv* env, jobject activity)
{
    if (!env || !activity)
        return {};

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getCodeCacheDir = activityClass ? env->GetMethodID(activityClass, "getCodeCacheDir", "()Ljava/io/File;") : nullptr;
    jobject cacheDir = (getCodeCacheDir && !ClearException(env))
        ? env->CallObjectMethod(activity, getCodeCacheDir) : nullptr;
    ClearException(env);

    if (!cacheDir)
    {
        jmethodID getCacheDir = activityClass ? env->GetMethodID(activityClass, "getCacheDir", "()Ljava/io/File;") : nullptr;
        cacheDir = (getCacheDir && !ClearException(env))
            ? env->CallObjectMethod(activity, getCacheDir) : nullptr;
        ClearException(env);
    }

    std::string result = GetFileAbsolutePath(env, cacheDir);
    if (cacheDir)
        env->DeleteLocalRef(cacheDir);
    if (activityClass)
        env->DeleteLocalRef(activityClass);
    return result;
}

bool WriteBridgeDex(const std::string& cacheDirPath, std::string& outDexPath)
{
    if (cacheDirPath.empty())
        return false;

    outDexPath = cacheDirPath + "/andhook_ime_bridge.dex";
    std::remove(outDexPath.c_str());

    std::ofstream file(outDexPath, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;

    file.write(reinterpret_cast<const char*>(kImeBridgeDex), static_cast<std::streamsize>(kImeBridgeDexSize));
    file.close();
    if (!file.good())
        return false;

    chmod(outDexPath.c_str(), 0444);
    return true;
}

bool EnsureRunnableBridgeLocked(JNIEnv* env, jobject activity)
{
    if (g_RunnableClass && g_RunnableCtor)
        return true;
    if (g_BridgeFailed || !env || !activity)
        return false;

    std::string cacheDirPath = GetCacheDirPath(env, activity);
    std::string dexPath;
    if (!WriteBridgeDex(cacheDirPath, dexPath))
    {
        g_BridgeFailed = true;
        LOGE("[ImGuiAndroidIme] failed to write bridge dex");
        return false;
    }

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader = activityClass ? env->GetMethodID(
        activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject parentLoader = (getClassLoader && !ClearException(env))
        ? env->CallObjectMethod(activity, getClassLoader) : nullptr;
    ClearException(env);
    if (activityClass)
        env->DeleteLocalRef(activityClass);
    if (!parentLoader)
    {
        g_BridgeFailed = true;
        return false;
    }

    jclass dexLoaderClass = env->FindClass("dalvik/system/DexClassLoader");
    jmethodID dexLoaderCtor = dexLoaderClass ? env->GetMethodID(
        dexLoaderClass, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V") : nullptr;
    jstring dexPathString = env->NewStringUTF(dexPath.c_str());
    jstring optDirString = env->NewStringUTF(cacheDirPath.c_str());
    jobject dexLoader = (dexLoaderCtor && dexPathString && optDirString && !ClearException(env))
        ? env->NewObject(dexLoaderClass, dexLoaderCtor, dexPathString, optDirString, nullptr, parentLoader) : nullptr;
    ClearException(env);

    jclass classLoaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = classLoaderClass ? env->GetMethodID(classLoaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jstring className = env->NewStringUTF("com.andhook.ImeBridgeRunnable");
    jclass localClass = (dexLoader && loadClass && className && !ClearException(env))
        ? static_cast<jclass>(env->CallObjectMethod(dexLoader, loadClass, className)) : nullptr;
    ClearException(env);
    jstring watcherClassName = env->NewStringUTF("com.andhook.ImeBridgeTextWatcher");
    jclass localWatcherClass = (dexLoader && loadClass && watcherClassName && !ClearException(env))
        ? static_cast<jclass>(env->CallObjectMethod(dexLoader, loadClass, watcherClassName)) : nullptr;
    ClearException(env);
    jstring editTextClassName = env->NewStringUTF("com.andhook.ImeBridgeEditText");
    jclass localEditTextClass = (dexLoader && loadClass && editTextClassName && !ClearException(env))
        ? static_cast<jclass>(env->CallObjectMethod(dexLoader, loadClass, editTextClassName)) : nullptr;
    ClearException(env);
    jstring inputConnectionClassName = env->NewStringUTF("com.andhook.ImeBridgeInputConnection");
    jclass localInputConnectionClass = (dexLoader && loadClass && inputConnectionClassName && !ClearException(env))
        ? static_cast<jclass>(env->CallObjectMethod(dexLoader, loadClass, inputConnectionClassName)) : nullptr;
    ClearException(env);

    if (localClass)
    {
        JNINativeMethod methods[] = {
            {const_cast<char*>("nativeRun"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(&NativeRun)},
        };
        if (env->RegisterNatives(localClass, methods, 1) != JNI_OK || ClearException(env))
            localClass = nullptr;
    }

    if (localClass)
    {
        g_RunnableClass = static_cast<jclass>(env->NewGlobalRef(localClass));
        g_RunnableCtor = env->GetMethodID(g_RunnableClass, "<init>", "(J)V");
        if (!g_RunnableClass || !g_RunnableCtor || ClearException(env))
        {
            if (g_RunnableClass)
                env->DeleteGlobalRef(g_RunnableClass);
            g_RunnableClass = nullptr;
            g_RunnableCtor = nullptr;
        }
    }

    if (localWatcherClass)
    {
        JNINativeMethod methods[] = {
            {const_cast<char*>("nativeTextChanged"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(&NativeTextChanged)},
        };
        if (env->RegisterNatives(localWatcherClass, methods, 1) != JNI_OK || ClearException(env))
            localWatcherClass = nullptr;
    }

    if (localWatcherClass)
    {
        g_TextWatcherClass = static_cast<jclass>(env->NewGlobalRef(localWatcherClass));
        g_TextWatcherCtor = env->GetMethodID(g_TextWatcherClass, "<init>", "()V");
        if (!g_TextWatcherClass || !g_TextWatcherCtor || ClearException(env))
        {
            if (g_TextWatcherClass)
                env->DeleteGlobalRef(g_TextWatcherClass);
            g_TextWatcherClass = nullptr;
            g_TextWatcherCtor = nullptr;
        }
    }

    if (localInputConnectionClass)
    {
        JNINativeMethod methods[] = {
            {const_cast<char*>("nativeDeleteBackward"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(&NativeDeleteBackward)},
            {const_cast<char*>("nativeTextChanged"), const_cast<char*>("(Ljava/lang/String;)V"), reinterpret_cast<void*>(&NativeTextChanged)},
        };
        if (env->RegisterNatives(localInputConnectionClass, methods, 2) != JNI_OK || ClearException(env))
            localInputConnectionClass = nullptr;
    }

    if (localEditTextClass)
    {
        g_EditTextClass = static_cast<jclass>(env->NewGlobalRef(localEditTextClass));
        g_EditTextCtor = env->GetMethodID(g_EditTextClass, "<init>", "(Landroid/content/Context;)V");
        if (!g_EditTextClass || !g_EditTextCtor || ClearException(env))
        {
            if (g_EditTextClass)
                env->DeleteGlobalRef(g_EditTextClass);
            g_EditTextClass = nullptr;
            g_EditTextCtor = nullptr;
        }
    }

    if (inputConnectionClassName)
        env->DeleteLocalRef(inputConnectionClassName);
    if (localInputConnectionClass)
        env->DeleteLocalRef(localInputConnectionClass);
    if (editTextClassName)
        env->DeleteLocalRef(editTextClassName);
    if (localEditTextClass)
        env->DeleteLocalRef(localEditTextClass);
    if (watcherClassName)
        env->DeleteLocalRef(watcherClassName);
    if (localWatcherClass)
        env->DeleteLocalRef(localWatcherClass);
    if (className)
        env->DeleteLocalRef(className);
    if (localClass)
        env->DeleteLocalRef(localClass);
    if (classLoaderClass)
        env->DeleteLocalRef(classLoaderClass);
    if (dexLoader)
        env->DeleteLocalRef(dexLoader);
    if (optDirString)
        env->DeleteLocalRef(optDirString);
    if (dexPathString)
        env->DeleteLocalRef(dexPathString);
    if (dexLoaderClass)
        env->DeleteLocalRef(dexLoaderClass);
    env->DeleteLocalRef(parentLoader);

    if (!g_RunnableClass || !g_RunnableCtor || !g_TextWatcherClass || !g_TextWatcherCtor ||
        !g_EditTextClass || !g_EditTextCtor)
    {
        g_BridgeFailed = true;
        LOGE("[ImGuiAndroidIme] failed to load bridge runnable");
        return false;
    }

    return true;
}

bool PostUiTask(UiTask* task)
{
    if (!task)
        return false;

    bool needDetach = false;
    JNIEnv* env = GetEnv(needDetach);
    if (!env)
    {
        delete task;
        return false;
    }

    jobject activity = AndroidPlatform::GetActivity();
    bool ok = false;
    if (activity)
    {
        std::lock_guard<std::mutex> lock(g_BridgeMutex);
        ok = EnsureRunnableBridgeLocked(env, activity);
    }

    if (ok)
    {
        jobject runnable = env->NewObject(g_RunnableClass, g_RunnableCtor,
            static_cast<jlong>(reinterpret_cast<intptr_t>(task)));
        ClearException(env);

        jclass activityClass = env->GetObjectClass(activity);
        jmethodID runOnUiThread = activityClass ? env->GetMethodID(
            activityClass, "runOnUiThread", "(Ljava/lang/Runnable;)V") : nullptr;
        if (runnable && runOnUiThread && !ClearException(env))
            env->CallVoidMethod(activity, runOnUiThread, runnable);
        ok = !ClearException(env);

        if (activityClass)
            env->DeleteLocalRef(activityClass);
        if (runnable)
            env->DeleteLocalRef(runnable);
    }

    DetachEnv(needDetach);

    if (!ok)
        delete task;
    return ok;
}

void SetViewBool(JNIEnv* env, jobject view, const char* methodName, bool value)
{
    jclass viewClass = env->GetObjectClass(view);
    jmethodID method = viewClass ? env->GetMethodID(viewClass, methodName, "(Z)V") : nullptr;
    if (method && !ClearException(env))
        env->CallVoidMethod(view, method, value ? JNI_TRUE : JNI_FALSE);
    ClearException(env);
    if (viewClass)
        env->DeleteLocalRef(viewClass);
}

void SetViewInt(JNIEnv* env, jobject view, const char* methodName, jint value)
{
    jclass viewClass = env->GetObjectClass(view);
    jmethodID method = viewClass ? env->GetMethodID(viewClass, methodName, "(I)V") : nullptr;
    if (method && !ClearException(env))
        env->CallVoidMethod(view, method, value);
    ClearException(env);
    if (viewClass)
        env->DeleteLocalRef(viewClass);
}

void SetViewFloat(JNIEnv* env, jobject view, const char* methodName, jfloat value)
{
    jclass viewClass = env->GetObjectClass(view);
    jmethodID method = viewClass ? env->GetMethodID(viewClass, methodName, "(F)V") : nullptr;
    if (method && !ClearException(env))
    {
        jvalue args[1]{};
        args[0].f = value;
        env->CallVoidMethodA(view, method, args);
    }
    ClearException(env);
    if (viewClass)
        env->DeleteLocalRef(viewClass);
}

jobject CreateLayoutParams(JNIEnv* env)
{
    jclass lpClass = env->FindClass("android/widget/FrameLayout$LayoutParams");
    jmethodID ctor = lpClass ? env->GetMethodID(lpClass, "<init>", "(II)V") : nullptr;
    jobject lp = (ctor && !ClearException(env)) ? env->NewObject(lpClass, ctor, 1, 1) : nullptr;
    ClearException(env);
    if (lpClass)
        env->DeleteLocalRef(lpClass);

    if (lp)
        return lp;

    lpClass = env->FindClass("android/view/ViewGroup$LayoutParams");
    ctor = lpClass ? env->GetMethodID(lpClass, "<init>", "(II)V") : nullptr;
    lp = (ctor && !ClearException(env)) ? env->NewObject(lpClass, ctor, 1, 1) : nullptr;
    ClearException(env);
    if (lpClass)
        env->DeleteLocalRef(lpClass);
    return lp;
}

bool EnsureEditTextOnUiThread(JNIEnv* env, jobject activity)
{
    if (g_EditText)
        return true;
    if (!env || !activity)
        return false;

    jclass editTextClass = g_EditTextClass;
    jobject editText = (editTextClass && g_EditTextCtor && !ClearException(env))
        ? env->NewObject(editTextClass, g_EditTextCtor, activity) : nullptr;
    if (!editText || ClearException(env))
        return false;

    SetViewBool(env, editText, "setFocusable", true);
    SetViewBool(env, editText, "setFocusableInTouchMode", true);
    SetViewBool(env, editText, "setSingleLine", true);
    SetViewBool(env, editText, "setSelectAllOnFocus", false);
    SetViewInt(env, editText, "setTextColor", 0x00000000);
    SetViewInt(env, editText, "setHintTextColor", 0x00000000);
    SetViewInt(env, editText, "setBackgroundColor", 0x00000000);
    SetViewFloat(env, editText, "setAlpha", 0.01f);

    jmethodID setInputType = editTextClass ? env->GetMethodID(editTextClass, "setInputType", "(I)V") : nullptr;
    if (setInputType && !ClearException(env))
        env->CallVoidMethod(editText, setInputType, kInputTypeText | kInputTypeNoSuggestions);
    ClearException(env);

    jmethodID setImeOptions = editTextClass ? env->GetMethodID(editTextClass, "setImeOptions", "(I)V") : nullptr;
    if (setImeOptions && !ClearException(env))
        env->CallVoidMethod(editText, setImeOptions, kImeActionDone | kImeFlagNoExtractUi);
    ClearException(env);

    if (g_TextWatcherClass && g_TextWatcherCtor)
    {
        jobject watcher = env->NewObject(g_TextWatcherClass, g_TextWatcherCtor);
        ClearException(env);
        jmethodID addTextChangedListener = editTextClass ? env->GetMethodID(
            editTextClass, "addTextChangedListener", "(Landroid/text/TextWatcher;)V") : nullptr;
        if (watcher && addTextChangedListener && !ClearException(env))
            env->CallVoidMethod(editText, addTextChangedListener, watcher);
        ClearException(env);

        if (watcher)
        {
            g_TextWatcher = env->NewGlobalRef(watcher);
            env->DeleteLocalRef(watcher);
        }
    }

    jobject layoutParams = CreateLayoutParams(env);
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID addContentView = activityClass ? env->GetMethodID(
        activityClass, "addContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V") : nullptr;
    if (addContentView && layoutParams && !ClearException(env))
        env->CallVoidMethod(activity, addContentView, editText, layoutParams);
    bool added = !ClearException(env);

    if (added)
    {
        g_EditText = env->NewGlobalRef(editText);
        LOGI("[ImGuiAndroidIme] UI-thread EditText ready");
    }

    if (activityClass)
        env->DeleteLocalRef(activityClass);
    if (layoutParams)
        env->DeleteLocalRef(layoutParams);
    env->DeleteLocalRef(editText);
    return added && g_EditText;
}

void SetEditTextString(JNIEnv* env, const std::string& text)
{
    if (!g_EditText)
        return;

    jstring jtext = env->NewStringUTF(text.c_str());
    if (!jtext || ClearException(env))
        return;

    jclass editTextClass = env->GetObjectClass(g_EditText);
    jmethodID setText = editTextClass ? env->GetMethodID(
        editTextClass, "setText", "(Ljava/lang/CharSequence;)V") : nullptr;
    if (setText && !ClearException(env))
        env->CallVoidMethod(g_EditText, setText, jtext);
    ClearException(env);

    jmethodID setSelection = editTextClass ? env->GetMethodID(editTextClass, "setSelection", "(I)V") : nullptr;
    if (setSelection && !ClearException(env))
        env->CallVoidMethod(g_EditText, setSelection, env->GetStringLength(jtext));
    ClearException(env);

    if (editTextClass)
        env->DeleteLocalRef(editTextClass);
    env->DeleteLocalRef(jtext);
}

void NativeDeleteBackward(JNIEnv* env, jclass, jint count)
{
    std::string text = ReadSharedText();
    int eraseCount = std::max(1, static_cast<int>(count));
    while (eraseCount-- > 0 && !text.empty())
        EraseLastUtf8Codepoint(text);

    UpdateSharedText(text);
    SetEditTextString(env, text);
}

std::string GetEditTextString(JNIEnv* env)
{
    if (!g_EditText)
        return {};

    jclass editTextClass = env->GetObjectClass(g_EditText);
    jmethodID getText = editTextClass ? env->GetMethodID(editTextClass, "getText", "()Landroid/text/Editable;") : nullptr;
    jobject editable = (getText && !ClearException(env)) ? env->CallObjectMethod(g_EditText, getText) : nullptr;
    ClearException(env);
    if (!editable)
    {
        if (editTextClass)
            env->DeleteLocalRef(editTextClass);
        return {};
    }

    jclass objectClass = env->FindClass("java/lang/Object");
    jmethodID toString = objectClass ? env->GetMethodID(objectClass, "toString", "()Ljava/lang/String;") : nullptr;
    jstring jtext = (toString && !ClearException(env)) ? static_cast<jstring>(env->CallObjectMethod(editable, toString)) : nullptr;
    ClearException(env);

    std::string result = JavaStringToStdString(env, jtext);
    if (jtext)
        env->DeleteLocalRef(jtext);
    if (objectClass)
        env->DeleteLocalRef(objectClass);
    env->DeleteLocalRef(editable);
    if (editTextClass)
        env->DeleteLocalRef(editTextClass);
    return result;
}

jobject GetSystemService(JNIEnv* env, jobject activity, const char* name)
{
    if (!env || !activity)
        return nullptr;

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getSystemService = activityClass ? env->GetMethodID(
        activityClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
    jstring serviceName = env->NewStringUTF(name);
    jobject service = (getSystemService && serviceName && !ClearException(env))
        ? env->CallObjectMethod(activity, getSystemService, serviceName) : nullptr;
    ClearException(env);

    if (serviceName)
        env->DeleteLocalRef(serviceName);
    if (activityClass)
        env->DeleteLocalRef(activityClass);
    return service;
}

void ShowKeyboardOnUiThread(JNIEnv* env, jobject activity, const std::string& text, bool syncText)
{
    if (!EnsureEditTextOnUiThread(env, activity))
    {
        g_ShowTaskPending.store(false);
        return;
    }

    if (syncText)
    {
        SetEditTextString(env, text);
        UpdateSharedText(text);
    }

    jclass editTextClass = env->GetObjectClass(g_EditText);
    jmethodID requestFocus = editTextClass ? env->GetMethodID(editTextClass, "requestFocus", "()Z") : nullptr;
    if (requestFocus && !ClearException(env))
        env->CallBooleanMethod(g_EditText, requestFocus);
    ClearException(env);

    jobject imm = GetSystemService(env, activity, "input_method");
    if (imm)
    {
        jclass immClass = env->GetObjectClass(imm);
        jmethodID restartInput = immClass ? env->GetMethodID(immClass, "restartInput", "(Landroid/view/View;)V") : nullptr;
        if (restartInput && !ClearException(env))
            env->CallVoidMethod(imm, restartInput, g_EditText);
        ClearException(env);

        jmethodID showSoftInput = immClass ? env->GetMethodID(immClass, "showSoftInput", "(Landroid/view/View;I)Z") : nullptr;
        if (showSoftInput && !ClearException(env))
            env->CallBooleanMethod(imm, showSoftInput, g_EditText, kShowForced);
        ClearException(env);

        if (immClass)
            env->DeleteLocalRef(immClass);
        env->DeleteLocalRef(imm);
    }

    if (editTextClass)
        env->DeleteLocalRef(editTextClass);
    g_ShowTaskPending.store(false);
}

void PollTextOnUiThread(JNIEnv* env)
{
    if (g_EditText)
        UpdateSharedText(GetEditTextString(env));
    g_PollTaskPending.store(false);
}

void SetTextOnUiThread(JNIEnv* env, const std::string& text)
{
    if (g_EditText)
        SetEditTextString(env, text);
}

void NativeRun(JNIEnv* env, jclass, jlong taskPtr)
{
    auto* task = reinterpret_cast<UiTask*>(static_cast<intptr_t>(taskPtr));
    if (!task)
        return;

    if (env->PushLocalFrame(64) != 0)
    {
        delete task;
        return;
    }

    jobject activity = AndroidPlatform::GetActivity();
    switch (task->type)
    {
    case UiTaskType::ShowKeyboard:
        ShowKeyboardOnUiThread(env, activity, task->text, task->syncText);
        break;
    case UiTaskType::PollText:
        PollTextOnUiThread(env);
        break;
    case UiTaskType::SetText:
        SetTextOnUiThread(env, task->text);
        break;
    }

    ClearException(env);
    env->PopLocalFrame(nullptr);
    delete task;
}

void NativeTextChanged(JNIEnv* env, jclass, jstring text)
{
    UpdateSharedText(JavaStringToStdString(env, text));
}

void RequestShowKeyboard(const std::string& text, bool syncText)
{
    if (g_ShowTaskPending.exchange(true))
        return;

    if (!PostUiTask(new UiTask{UiTaskType::ShowKeyboard, text, syncText}))
        g_ShowTaskPending.store(false);
}

void RequestPollText()
{
    if (g_PollTaskPending.exchange(true))
        return;

    if (!PostUiTask(new UiTask{UiTaskType::PollText, {}}))
        g_PollTaskPending.store(false);
}

void RequestSetText(const std::string& text)
{
    PostUiTask(new UiTask{UiTaskType::SetText, text, false});
}

} // namespace

bool InputText(const char* label, char* buffer, size_t bufferSize, ImGuiInputTextFlags flags)
{
    if (!label || !buffer || bufferSize == 0)
        return false;

    ImGuiID id = ImGui::GetID(label);
    bool changedByIme = false;
    if (g_ActiveWidget == id)
    {
        changedByIme = CopyTextToBuffer(ReadSharedText(), buffer, bufferSize);
        if (changedByIme)
            ReloadActiveInputState(id);
    }

    ImGuiInputTextFlags displayFlags = flags | ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo;
    ImGui::InputText(label, buffer, bufferSize, displayFlags);
    bool activated = ImGui::IsItemActivated();
    bool clicked = ImGui::IsItemClicked();

    if (activated || clicked)
    {
        bool wasActive = (g_ActiveWidget == id);
        g_ActiveWidget = id;
        g_NextPollTime = 0.0;
        RequestShowKeyboard(buffer, !wasActive);
    }

    if (g_ActiveWidget == id)
    {
        double now = ImGui::GetTime();
        if (now >= g_NextPollTime)
        {
            RequestPollText();
            g_NextPollTime = now + kPollIntervalSeconds;
        }
    }

    return changedByIme;
}

void DeleteBackward()
{
    if (g_ActiveWidget == 0)
        return;

    std::string text = ReadSharedText();
    if (text.empty())
        return;

    EraseLastUtf8Codepoint(text);
    UpdateSharedText(text);
    RequestSetText(text);
}

void SetText(const char* text)
{
    std::string value = text ? text : "";
    UpdateSharedText(value);
    RequestSetText(value);
}

void HideKeyboard()
{
    g_ShowTaskPending.store(false);
    g_PollTaskPending.store(false);
    g_ActiveWidget = 0;
    g_NextPollTime = 0.0;
}

} // namespace ImGuiAndroidIme
