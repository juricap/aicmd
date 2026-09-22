#include "session.h"
#include <algorithm>
#include <cmath>
#include <cctype>
namespace av {
Kind kind(const ac::fs::path& p) {
    auto e = p.extension().u8string();
    std::transform(e.begin(),e.end(),e.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".bmp" || e == ".webp") return Kind::Image;
    if (e == ".mp4" || e == ".m4v" || e == ".mov" || e == ".mkv" || e == ".avi" || e == ".webm" || e == ".mts") return Kind::Video;
    if (e == ".mp3" || e == ".wav" || e == ".flac" || e == ".m4a" || e == ".ogg" || e == ".opus" || e == ".aac") return Kind::Audio;
    return Kind::Unknown;
}
void Session::replace(std::vector<Item> list,size_t start) {
    if (list.empty() || list.size() > 100000 || start >= list.size()) throw std::runtime_error("Invalid media folder snapshot");
    items = std::move(list); index = start; ++generation;
}
const Item& Session::current() const { if (items.empty()) throw std::runtime_error("No media session"); return items[index]; }
void Session::validate(const Command& c) const {
    current();
    if (c.generation && c.generation != generation) throw std::runtime_error("Stale media generation; get state again");
    if (!std::isfinite(c.value)) throw std::runtime_error("Value must be finite");
    if (c.action == Action::Select && (c.value < 0 || c.value >= items.size() || std::floor(c.value) != c.value)) throw std::runtime_error("Invalid item index");
    if (c.action == Action::StepFrame && (current().type != Kind::Video || (c.value != 1 && c.value != -1))) throw std::runtime_error("Frame step requires video and direction +1 or -1");
    if ((c.action == Action::SeekRelative || c.action == Action::SeekAbsolute) && current().type == Kind::Image) throw std::runtime_error("Still images have no playback timeline");
    if ((c.action == Action::CopyFrame || c.action == Action::CaptureFrame) && current().type == Kind::Audio) throw std::runtime_error("Audio has no image to capture");
    if (c.action == Action::Volume && (c.value < 0 || c.value > 100)) throw std::runtime_error("Volume must be 0..100");
    if (c.action == Action::ImageZoom && (current().type != Kind::Image || c.value < -10 || c.value > 10)) throw std::runtime_error("Image zoom must be -10..10 on a still image");
}
bool Session::navigate(const Command& c) {
    validate(c); auto next = index;
    switch (c.action) {
    case Action::Next: if (next+1 < items.size()) ++next; break;
    case Action::Previous: if (next) --next; break;
    case Action::First: next = 0; break;
    case Action::Last: next = items.size()-1; break;
    case Action::Select: next = static_cast<size_t>(c.value); break;
    default: return false;
    }
    if (index == next) return false;
    index = next; ++generation; return true;
}
std::vector<Item> folder(const ac::Location& file,const ac::Cancel& cancel) {
    auto listing = ac::filesystem_provider()->list(file.parent_path(),false,cancel,32*1024*1024);
    if (listing.error) throw std::runtime_error(listing.error.message);
    ac::sort_entries(listing.entries,{}); std::vector<Item> result;
    for (const auto& e : listing.entries) {
        auto type = kind(e.name); auto location = file.parent_path()/e.name;
        if (!e.directory && ((!e.hidden && type != Kind::Unknown) || location == file)) result.push_back({location,type});
    }
    if (std::none_of(result.begin(),result.end(),[&](const Item& i){return i.location == file;})) result.push_back({file,kind(file.filename())});
    return result;
}
}
