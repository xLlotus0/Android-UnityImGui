#pragma once
#include <android/looper.h>

#include <functional>
#include <mutex>
#include <queue>

class LooperDispatcher
{
public:
    LooperDispatcher() = default;
    ~LooperDispatcher();

    LooperDispatcher(const LooperDispatcher &) = delete;
    LooperDispatcher &operator=(const LooperDispatcher &) = delete;

    // 在当前线程调用，绑定到当前线程的 ALooper
    void init();

    // 从任意线程 post 一个任务到其它线程
    void post(std::function<void()> task);

    void cleanup();

private:
    static int looper_callback(int fd, int events, void* data);

    int efd_ = -1;
    ALooper *looper_ = nullptr;
    std::mutex mtx_;
    std::queue<std::function<void()>> tasks_;
    bool initialized_ = false;
};
