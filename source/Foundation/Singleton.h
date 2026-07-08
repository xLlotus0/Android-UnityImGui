#pragma once

template <typename T>
class Singleton {
public:
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;

    static T& GetInstance() {
        static T gInstance;
        return gInstance;
    }

protected:
    Singleton() = default;
    ~Singleton() = default;
};
