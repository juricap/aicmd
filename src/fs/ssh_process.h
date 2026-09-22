#pragma once
#include "core/core.h"
#include <chrono>
#include <functional>
namespace ac {
class SshPipe {
public:
    explicit SshPipe(const std::string& host);
    ~SshPipe();
    SshPipe(const SshPipe&) = delete;
    void read(char*,size_t);
    void write(const char*,size_t);
    void kill();
    std::string diagnostic();
    std::atomic<int64_t> activity{0};
    static int64_t now();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
// Interrupts blocked pipe I/O by killing only this SSH child, not other sessions.
class SshDeadline {
    SshPipe& pipe;
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    std::thread monitor;
public:
    SshDeadline(SshPipe&,Cancel);
    ~SshDeadline();
};
}
