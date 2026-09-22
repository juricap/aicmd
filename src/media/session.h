#pragma once
#include "core/core.h"
namespace av {
enum class Kind { Unknown, Image, Video, Audio };
Kind kind(const ac::fs::path&);
struct Item { ac::Location location; Kind type = Kind::Unknown; };
enum class Action { Next, Previous, First, Last, Select, Play, Pause, TogglePause,
    SeekRelative, SeekAbsolute, StepFrame, Volume, Mute, Fullscreen, ImageZoom, CopyFrame, CaptureFrame, Close };
struct Command { Action action; double value = 0; uint64_t generation = 0; };
// The native shell and model adapters share this controller; no transport/GUI/backend types.
class Session {
public:
    std::vector<Item> items;
    size_t index = 0;
    uint64_t generation = 0;
    void replace(std::vector<Item>,size_t);
    bool navigate(const Command&);
    void validate(const Command&) const;
    const Item& current() const;
};
std::vector<Item> folder(const ac::Location&,const ac::Cancel&);
}
