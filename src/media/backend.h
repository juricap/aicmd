#pragma once
#include "session.h"
#include <functional>
namespace av {
struct Pixels { int width = 0, height = 0; std::vector<unsigned char> bgra; };
struct Playback {
    bool ready = false, paused = false, seeking = false, ended = false;
    double position = 0, duration = 0, volume = 100;
    int width = 0,height = 0;
    bool muted = false;
    std::string error, decoder, version;
    uint64_t bytesRead = 0;
};
class Backend {
public:
    Backend(); ~Backend();
    void initialize(uintptr_t nativeWindow);
    void load(const Item&,uint64_t generation);
    void command(const std::vector<std::string>&);
    void capture(uint64_t request,uint64_t generation);
    void poll(const std::function<void(uint64_t,uint64_t,Pixels,std::string)>&);
    const Playback& state() const;
    void cancel();
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
}
