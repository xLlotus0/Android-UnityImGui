#include "LooperDispatcher.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include "Foundation/Logger.h"

LooperDispatcher::~LooperDispatcher()
{
    if (initialized_) {
        cleanup();
    }
}

void LooperDispatcher::init()
{
    looper_ = ALooper_forThread();
    if (!looper_)
    {
        looper_ = ALooper_prepare(0);
    }

    if (!looper_)
    {
        LOGD("[LooperDispatcher] init FAILED! looper is null");
        return;
    }

    efd_ = eventfd(0, EFD_NONBLOCK);
    if (efd_ < 0)
    {
        LOGD("[LooperDispatcher] init FAILED! eventfd creation failed");
        return;
    }

    ALooper_addFd(looper_, efd_, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, looper_callback, this);
    initialized_ = true;
    LOGD("[LooperDispatcher] initialized, eventfd=%d", efd_);
}

int LooperDispatcher::looper_callback(int fd, int /*events*/, void* data)
{
    auto* self = static_cast<LooperDispatcher*>(data);

    // 消费 eventfd 计数
    uint64_t val;
    read(fd, &val, sizeof(val));

    // 取出所有待执行任务
    std::queue<std::function<void()>> pending;
    {
        std::lock_guard lock(self->mtx_);
        pending.swap(self->tasks_);
    }

    while (!pending.empty())
    {
        pending.front()();
        pending.pop();
    }

    return 1; // 继续监听
}

void LooperDispatcher::post(std::function<void()> task)
{
    {
        std::lock_guard lock(mtx_);
        tasks_.push(std::move(task));
    }

    uint64_t val = 1;
    ssize_t written = write(efd_, &val, sizeof(val));
    if (written != sizeof(val)) {
        LOGD("[LooperDispatcher] post FAILED! written=%zd", written);
    }
}

void LooperDispatcher::cleanup()
{
    if (!initialized_) {
        LOGD("[LooperDispatcher] not initialized");
        return;
    }

    if (looper_ && efd_ >= 0) {
        ALooper_removeFd(looper_, efd_);
        LOGD("[LooperDispatcher] removed fd from looper");
    }

    if (efd_ >= 0) {
        close(efd_);
        efd_ = -1;
    }

    initialized_ = false;
    LOGD("[LooperDispatcher] cleanup completed");
}
