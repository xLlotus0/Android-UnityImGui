#include "ActivityHost.h"

#include "JniRuntime.h"
#include "Foundation/Logger.h"

namespace AndroidPlatform
{

namespace
{

jobject s_globalActivity = nullptr;

jobject ResolveAnyActivityLocal(JNIEnv* env)
{
    // 1. ActivityThread.currentActivityThread()
    jclass atClass = env->FindClass("android/app/ActivityThread");
    if (!atClass || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jmethodID catMethod = env->GetStaticMethodID(atClass, "currentActivityThread",
        "()Landroid/app/ActivityThread;");
    if (!catMethod || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jobject at = env->CallStaticObjectMethod(atClass, catMethod);
    if (!at || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    // 2. mActivities: ArrayMap<IBinder, ActivityClientRecord> (API 21+)
    jfieldID activitiesField = env->GetFieldID(atClass, "mActivities",
        "Landroid/util/ArrayMap;");
    if (!activitiesField || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    jobject activities = env->GetObjectField(at, activitiesField);
    if (!activities || env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }

    // 3. ArrayMap.size() / valueAt(i)
    jclass mapClass = env->GetObjectClass(activities);
    jmethodID sizeMethod    = env->GetMethodID(mapClass, "size", "()I");
    jmethodID valueAtMethod = env->GetMethodID(mapClass, "valueAt", "(I)Ljava/lang/Object;");
    if (!sizeMethod || !valueAtMethod) return nullptr;

    jint size = env->CallIntMethod(activities, sizeMethod);
    if (size <= 0) return nullptr;

    // Activity.isFinishing() —— 优先选非 finishing 的 Activity
    jclass actClass = env->FindClass("android/app/Activity");
    jmethodID isFinishingMethod = (actClass && !env->ExceptionCheck())
        ? env->GetMethodID(actClass, "isFinishing", "()Z")
        : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    jobject firstActivity   = nullptr;  // 兜底：第一个非 null 的 Activity
    jobject runningActivity = nullptr;  // 优先：第一个 !isFinishing() 的 Activity

    for (jint i = 0; i < size; ++i)
    {
        jobject record = env->CallObjectMethod(activities, valueAtMethod, i);
        if (!record) continue;

        jclass recClass = env->GetObjectClass(record);
        jfieldID actField = env->GetFieldID(recClass, "activity", "Landroid/app/Activity;");
        if (!actField || env->ExceptionCheck()) { env->ExceptionClear(); continue; }

        jobject activity = env->GetObjectField(record, actField);
        if (!activity) continue;

        if (!firstActivity)
            firstActivity = activity;

        if (isFinishingMethod)
        {
            jboolean finishing = env->CallBooleanMethod(activity, isFinishingMethod);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!finishing)
            {
                runningActivity = activity;
                break;
            }
        }
        else
        {
            runningActivity = activity;
            break;
        }
    }

    return runningActivity ? runningActivity : firstActivity;
}

} // anonymous namespace

jobject GetActivity()
{
    JavaVM* vm = GetJavaVM();
    if (!vm)
        return nullptr;

    JNIEnv* env       = nullptr;
    bool    needDetach = false;

    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            LOGE("[ActivityHost] AttachCurrentThread failed");
            return nullptr;
        }
        needDetach = true;
    }

    if (env->PushLocalFrame(32) == 0)
    {
        jobject local = ResolveAnyActivityLocal(env);
        if (local)
        {
            if (!s_globalActivity || !env->IsSameObject(s_globalActivity, local))
            {
                if (s_globalActivity)
                    env->DeleteGlobalRef(s_globalActivity);
                s_globalActivity = env->NewGlobalRef(local);
                LOGI("[ActivityHost] Activity=%p", s_globalActivity);
            }
        }
        env->PopLocalFrame(nullptr);
    }

    if (env->ExceptionCheck())
        env->ExceptionClear();

    if (needDetach)
        vm->DetachCurrentThread();

    return s_globalActivity;
}

}
