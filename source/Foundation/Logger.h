#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef kANDROID_LOG
#include <android/log.h>
#else
// 非 Android：复刻 Android 的 prio 常量，让 Category API 跨平台编译。
// 数值与 android/log.h 中 android_LogPriority 一致。
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO  4
#define ANDROID_LOG_WARN  5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#endif

#ifndef kANDROID_LOG_TAG
#define kANDROID_LOG_TAG "INJECT"
#endif

#ifndef kPROJECT_NAME
#define kPROJECT_NAME "dfmhook"
#endif

// =============================================================================
//  时间戳 helper
// =============================================================================
inline std::string FormatedTime()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_r(&time_t_now, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

inline std::string FormatedTimeShort()
{
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm;
    localtime_r(&time_t_now, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%m-%d_%H-%M-%S");
    return oss.str();
}

// =============================================================================
//  LogFile：副线 per-tag 文件（Category 的 file_tag、CrashHandler 等使用）
//  std::ios::app 模式让每次 write 自动落到末尾，无须 seek。
// =============================================================================
class LogFile {
public:
    explicit LogFile(std::filesystem::path path) : path_(std::move(path)) {
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        file_.open(path_, std::ios::binary | std::ios::app);
    }

    void Append(const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.write(data.data(), data.size());
            file_.flush();
        }
    }

    template <typename... Args>
    void Append(std::format_string<Args...> fmt, Args&&... args) {
        Append(std::format(fmt, std::forward<Args>(args)...));
    }

private:
    std::filesystem::path path_;
    std::ofstream         file_;
    std::mutex            mutex_;
};

inline LogFile* GetLogFile(const std::string& dir)
{
    namespace fs = std::filesystem;
    static std::unordered_map<std::string, std::unique_ptr<LogFile>> g_log_files;
    static std::mutex g_log_files_mutex;
    std::lock_guard<std::mutex> lock(g_log_files_mutex);
    auto [it, inserted] = g_log_files.try_emplace(dir, nullptr);
    if (inserted)
    {
        const char* ext = std::getenv("EXTERNAL_STORAGE");
        fs::path saved_path = fs::path(ext && ext[0] ? ext : "/sdcard") / "Android" / "data" / getprogname() / "cache";
        fs::path final_path = saved_path / kPROJECT_NAME / dir;
        fs::path file_name = dir + "_" + FormatedTimeShort() + ".log";
        it->second = std::make_unique<LogFile>(final_path / file_name);
    }
    return it->second.get();
}

// =============================================================================
//  BufferedLog：Category 主时间线后端
//
//  - 单互斥锁串行化对 buffer / FILE* 的访问
//  - 达到 kFlushThreshold（16 KB）才 fwrite + fflush，让大批连续日志走一次系统调用
//  - logd 写在 mutex 释放后，不阻塞其他生产者；它是 best-effort，可丢
//  - 单文件超过 kPerFileMaxBytes（32 MB）滚到 _002 / _003 …；同会话最多
//    kMaxFilesPerSession 份；超出后停止落盘，logd 仍走
//  - 启动时执行一次目录清理：保留最新 kRetainTotalFiles 份 Logger_*.log
//  - State 为函数局部 static，首次 Emit 时初始化；atexit 注册一次 Flush 兜底
//  - 路径：/sdcard/Android/data/<pkg>/cache/<PROJECT>/Logger/Logger_<ts>_<seq>.log
//
//  全部 inline 在 header 里；具名 detail::State 配合 inline 函数局部 static，
//  确保跨 TU 共享同一个 State 实例（C++17 ODR 保证）。
// =============================================================================
namespace BufferedLog {
namespace detail {

constexpr size_t kFlushThreshold     = 16 * 1024;
constexpr size_t kPerFileMaxBytes    = 32 * 1024 * 1024;
constexpr int    kMaxFilesPerSession = 4;
constexpr int    kRetainTotalFiles   = 12;

inline char PrioChar(int prio)
{
#ifdef kANDROID_LOG
    switch (prio)
    {
        case ANDROID_LOG_DEBUG: return 'D';
        case ANDROID_LOG_INFO:  return 'I';
        case ANDROID_LOG_WARN:  return 'W';
        case ANDROID_LOG_ERROR: return 'E';
        case ANDROID_LOG_FATAL: return 'F';
        default:                return '?';
    }
#else
    (void)prio; return '?';
#endif
}

// "[I 12:34:56.123] " — 时间戳精确到毫秒，便于排序与对齐 logcat
inline size_t WritePrefix(char* out, size_t cap, int prio)
{
    using namespace std::chrono;
    auto now    = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    auto ms     = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm;
    localtime_r(&t, &tm);
    return static_cast<size_t>(std::snprintf(
        out, cap, "[%c %02d:%02d:%02d.%03lld] ",
        PrioChar(prio), tm.tm_hour, tm.tm_min, tm.tm_sec,
        static_cast<long long>(ms)));
}

class State
{
public:
    static State& Instance()
    {
        static State* inst = new State();  // 进程级泄漏，避免析构序问题
        return *inst;
    }

    void Emit(int prio, std::string_view msg)
    {
        // 1) 内存缓冲（lossless 路径）
        {
            std::lock_guard<std::mutex> lk(mu_);

            char head[40];
            size_t n = WritePrefix(head, sizeof(head), prio);
            if (n > sizeof(head)) n = sizeof(head);
            buffer_.append(head, n);
            buffer_.append(msg.data(), msg.size());
            if (msg.empty() || msg.back() != '\n')
                buffer_.push_back('\n');

            if (buffer_.size() >= kFlushThreshold)
                FlushLocked();
        }

        // 2) logd best-effort（不持锁，避免拖慢其他生产者）
#ifdef kANDROID_LOG
        __android_log_print(prio, kANDROID_LOG_TAG, "%.*s",
                            static_cast<int>(msg.size()), msg.data());
#endif
    }

    void Flush()
    {
        std::lock_guard<std::mutex> lk(mu_);
        FlushLocked();
    }

private:
    State()
    {
        InitDirAndTimestamp();
        CleanupOldFiles();
        OpenSeqFile(1);
        std::atexit(&State::AtExit);
    }

    static void AtExit() { Instance().Flush(); }

    void InitDirAndTimestamp()
    {
        const char* ext = std::getenv("EXTERNAL_STORAGE");
        dir_ = std::filesystem::path(ext && *ext ? ext : "/sdcard")
            / "Android" / "data" / getprogname() / "cache" / kPROJECT_NAME / "Logger";

        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);

        std::time_t t = std::time(nullptr);
        std::tm tm;
        localtime_r(&t, &tm);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%m-%d_%H-%M-%S", &tm);
        timestamp_ = ts;
    }

    void CleanupOldFiles()
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        std::vector<std::pair<fs::file_time_type, fs::path>> files;
        for (const auto& entry : fs::directory_iterator(dir_, ec))
        {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const auto name = entry.path().filename().string();
            if (name.size() < std::string_view("Logger_X.log").size()) continue;
            if (name.compare(0, 7, "Logger_") != 0) continue;
            if (name.compare(name.size() - 4, 4, ".log") != 0) continue;
            auto mtime = entry.last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            files.emplace_back(mtime, entry.path());
        }

        if (files.size() <= kRetainTotalFiles) return;

        std::sort(files.begin(), files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        const size_t toDelete = files.size() - kRetainTotalFiles;
        for (size_t i = 0; i < toDelete; ++i)
        {
            fs::remove(files[i].second, ec);
            ec.clear();
        }
    }

    void OpenSeqFile(int seq)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "Logger_%s_%03d.log", timestamp_.c_str(), seq);
        std::filesystem::path full = dir_ / name;
        file_ = std::fopen(full.c_str(), "ab");
        seq_ = seq;
        bytes_in_current_file_ = 0;
    }

    void RotateIfNeeded()
    {
        if (!file_) return;
        if (bytes_in_current_file_ < kPerFileMaxBytes) return;

        std::fclose(file_);
        file_ = nullptr;

        if (seq_ >= kMaxFilesPerSession)
        {
#ifdef kANDROID_LOG
            __android_log_print(ANDROID_LOG_WARN, kANDROID_LOG_TAG,
                "[BufferedLog] session hit %d x %zu MB cap; further file writes dropped, logd still active",
                kMaxFilesPerSession, kPerFileMaxBytes / (1024 * 1024));
#endif
            return;
        }
        OpenSeqFile(seq_ + 1);
    }

    void FlushLocked()
    {
        if (buffer_.empty()) return;
        if (file_)
        {
            std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
            std::fflush(file_);
            bytes_in_current_file_ += buffer_.size();
            RotateIfNeeded();
        }
        buffer_.clear();
    }

    std::FILE*            file_ = nullptr;
    std::string           buffer_;
    std::mutex            mu_;
    std::filesystem::path dir_;
    std::string           timestamp_;
    int                   seq_ = 0;
    size_t                bytes_in_current_file_ = 0;
};

}  // namespace detail

inline void Emit(int android_priority, std::string_view msg)
{
    detail::State::Instance().Emit(android_priority, msg);
}

inline void Flush()
{
    detail::State::Instance().Flush();
}

}  // namespace BufferedLog

// =============================================================================
//  LogHelper：FLOG* / Category 用的工具
// =============================================================================
namespace LogHelper {

// 副线文件 / 非 Android 退化路径用：把 Android prio 映射到单字母前缀。
inline char prioChar(int prio) {
    switch (prio) {
        case ANDROID_LOG_DEBUG: return 'D';
        case ANDROID_LOG_INFO:  return 'I';
        case ANDROID_LOG_WARN:  return 'W';
        case ANDROID_LOG_ERROR: return 'E';
        case ANDROID_LOG_FATAL: return 'F';
        default:                return '?';
    }
}

// Debug 级的编译期裁剪开关。kNO_DEBUG_LOG 定义后，Debug 调用整段被 if constexpr 干掉。
constexpr bool isCompiledOut(int prio) {
#ifdef kNO_DEBUG_LOG
    return prio == ANDROID_LOG_DEBUG;
#else
    (void)prio;
    return false;
#endif
}

// FLOG* 后端
inline void emitStr(int prio, const char* msg) {
#ifdef kANDROID_LOG
    BufferedLog::Emit(prio, std::string_view(msg));
#else
    printf("%c: %s\n", prioChar(prio), msg);
#endif
}

// 类别（UE 风格 + 对象方法）
//
//  - name：行前缀里的标签，例如主时间线会出现 "[I 12:34:56.123] [VM] ..."
//  - file_tag：非空时同时把消息 Append 到 GetLogFile(file_tag) 指向的副线文件
//      （日期戳 + 级别名 + 消息），用于离线分析；为空时仅进主时间线
//
//  运行时字符串：写成 LogVM.Info("{}", runtime_str)，因为
//  std::format_string 需要 consteval 字面量。
struct Category;

inline void emitCategory(const Category& cat, int prio, bool sync, const std::string& msg);

struct Category {
    const char* name;
    const char* file_tag;

#define LOGCAT_METHOD_(MethodName, Prio, Sync)                                             \
    template<typename... Args>                                                             \
    inline void MethodName(std::format_string<Args...> fmt, Args&&... args) const {        \
        if constexpr (!isCompiledOut(Prio)) {                                              \
            emitCategory(*this, (Prio), (Sync),                                            \
                std::format(fmt, std::forward<Args>(args)...));                            \
        }                                                                                  \
    }

    LOGCAT_METHOD_(Debug, ANDROID_LOG_DEBUG, false)
    LOGCAT_METHOD_(Info,  ANDROID_LOG_INFO,  false)
    LOGCAT_METHOD_(Warn,  ANDROID_LOG_WARN,  false)
    LOGCAT_METHOD_(Error, ANDROID_LOG_ERROR, false)
    LOGCAT_METHOD_(Fatal, ANDROID_LOG_FATAL, false)

    LOGCAT_METHOD_(DebugSync, ANDROID_LOG_DEBUG, true)
    LOGCAT_METHOD_(InfoSync,  ANDROID_LOG_INFO,  true)
    LOGCAT_METHOD_(WarnSync,  ANDROID_LOG_WARN,  true)
    LOGCAT_METHOD_(ErrorSync, ANDROID_LOG_ERROR, true)
    LOGCAT_METHOD_(FatalSync, ANDROID_LOG_FATAL, true)

#undef LOGCAT_METHOD_
};

inline void emitCategory(const Category& cat, int prio, bool sync, const std::string& msg) {
    // 1) 主时间线（无损路径）
#ifdef kANDROID_LOG
    auto line = std::format("[{}] {}", cat.name, msg);
    BufferedLog::Emit(prio, line);
#else
    printf("%c: [%s] %s\n", prioChar(prio), cat.name, msg.c_str());
#endif

    // 2) 副线文件（仅显式声明 file_tag 的 category 才走）
    if (cat.file_tag) {
        GetLogFile(cat.file_tag)->Append("{}    [{}] {}\n",
            FormatedTime(), prioChar(prio), msg);
    }

    // 3) 强制刷盘（崩溃前最后一句）
    if (sync) {
        BufferedLog::Flush();
    }
}

}  // namespace LogHelper

// =============================================================================
//  printf 基底 —— 直接展开为裸 __android_log_print，不进 BufferedLog
//  logd 限流可丢；要无损请走 Category（LogXxx.Info / LOG_INFO 等）
//
//  LOGD 的 [文件名(行号) 函数名] 前缀走编译器内建：
//    __FILE_NAME__ 由 clang/gcc 在编译期算出 basename，运行时零开销
//    （NDK clang ≥ 9 / GCC ≥ 12 都支持）
// =============================================================================
#ifdef kANDROID_LOG

#ifndef kNO_DEBUG_LOG
#define LOGD(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, kANDROID_LOG_TAG, "[%s(%d) %s] " fmt, __FILE_NAME__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) do {} while(0)
#endif

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  kANDROID_LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) __android_log_print(ANDROID_LOG_WARN,  kANDROID_LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, kANDROID_LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGF(fmt, ...) __android_log_print(ANDROID_LOG_FATAL, kANDROID_LOG_TAG, fmt, ##__VA_ARGS__)

#else  // !kANDROID_LOG —— 退化为 stdout

#ifndef kNO_DEBUG_LOG
#define LOGD(fmt, ...) printf("D: [%s(%d) %s] " fmt "\n", __FILE_NAME__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...) do {} while(0)
#endif
#define LOGI(fmt, ...) printf("I: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) printf("W: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) printf("E: " fmt "\n", ##__VA_ARGS__)
#define LOGF(fmt, ...) printf("F: " fmt "\n", ##__VA_ARGS__)

#endif  // kANDROID_LOG

// =============================================================================
//  FLOG*（兼容层；新代码请用 Category 对象方法 LogXxx.Info(...) 等）
//  现有 ~137 处 FLOG* 调用维持工作；后续按模块分批迁移到 Category 方法。
// =============================================================================
#ifndef kNO_DEBUG_LOG
#define FLOGD(fmt, ...) LogHelper::emitStr(ANDROID_LOG_DEBUG, std::format(fmt, ##__VA_ARGS__).c_str())
#else
#define FLOGD(fmt, ...) do {} while(0)
#endif

#define FLOGI(fmt, ...) LogHelper::emitStr(ANDROID_LOG_INFO,  std::format(fmt, ##__VA_ARGS__).c_str())
#define FLOGW(fmt, ...) LogHelper::emitStr(ANDROID_LOG_WARN,  std::format(fmt, ##__VA_ARGS__).c_str())
#define FLOGE(fmt, ...) LogHelper::emitStr(ANDROID_LOG_ERROR, std::format(fmt, ##__VA_ARGS__).c_str())
#define FLOGF(fmt, ...) LogHelper::emitStr(ANDROID_LOG_FATAL, std::format(fmt, ##__VA_ARGS__).c_str())

// =============================================================================
//  类别声明 + 调用接口（对象方法 + 宏，两套并存）
//
//  声明：
//      DECLARE_LOG_CATEGORY      (LogTemp, "Temp");           // 仅主时间线
//      DECLARE_LOG_CATEGORY_FILE (LogVM,   "VM", "AceVM");    // 主线 + 副线文件
//
//  调用 —— 选其一即可，两者后端一致：
//
//    A) 对象方法（推荐，简洁）
//       LogVM.Debug ("step={} pc={:#x}", op, pc);
//       LogVM.Info  ("loaded {} symbols", n);
//       LogVM.Error ("fatal {}", x);
//       LogVM.FatalSync("crash code={}", code);   // 写完立即 Flush
//
//    B) 宏（热路径 / args 评估代价大时用，编译期完全裁剪 args）
//       LOG_DEBUG(LogVM, "step={} pc={:#x}", op, pc);
//       LOG_FATAL_SYNC(LogVM, "crash code={}", code);
//
//  差异：
//    - kNO_DEBUG_LOG 定义后，两者的 Debug 路径都被 if constexpr 干掉。
//    - 宏版本：args 表达式在 if constexpr 的 discarded branch 内，
//      不会被求值（runtime 0 cost）。
//    - 对象版：args 在进入方法前就求值，方法体只是空 return。
// =============================================================================
#define DECLARE_LOG_CATEGORY(VarName, NameStr) \
    inline constexpr ::LogHelper::Category VarName{ NameStr, nullptr }

#define DECLARE_LOG_CATEGORY_FILE(VarName, NameStr, FileTag) \
    inline constexpr ::LogHelper::Category VarName{ NameStr, FileTag }

// 内部派发宏：把 args 关在 if constexpr 里，Debug 在 kNO_DEBUG_LOG 时整段消失
#define LOG_INTERNAL_DISPATCH_(Cat, Prio, Sync, Fmt, ...) \
    do { \
        if constexpr (!::LogHelper::isCompiledOut(Prio)) { \
            ::LogHelper::emitCategory((Cat), (Prio), (Sync), \
                std::format(Fmt, ##__VA_ARGS__)); \
        } \
    } while(0)

#define LOG_DEBUG(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_DEBUG, false, Fmt, ##__VA_ARGS__)
#define LOG_INFO(Cat,  Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_INFO,  false, Fmt, ##__VA_ARGS__)
#define LOG_WARN(Cat,  Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_WARN,  false, Fmt, ##__VA_ARGS__)
#define LOG_ERROR(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_ERROR, false, Fmt, ##__VA_ARGS__)
#define LOG_FATAL(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_FATAL, false, Fmt, ##__VA_ARGS__)

#define LOG_DEBUG_SYNC(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_DEBUG, true, Fmt, ##__VA_ARGS__)
#define LOG_INFO_SYNC(Cat,  Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_INFO,  true, Fmt, ##__VA_ARGS__)
#define LOG_WARN_SYNC(Cat,  Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_WARN,  true, Fmt, ##__VA_ARGS__)
#define LOG_ERROR_SYNC(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_ERROR, true, Fmt, ##__VA_ARGS__)
#define LOG_FATAL_SYNC(Cat, Fmt, ...) LOG_INTERNAL_DISPATCH_(Cat, ANDROID_LOG_FATAL, true, Fmt, ##__VA_ARGS__)
