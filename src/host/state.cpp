#include "state.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif
namespace ac {
using Json = nlohmann::json;
static Json encode_path(const fs::path& path) {
    const char* hex = "0123456789abcdef";
    std::string raw;
    for (auto c : path.native()) {
        auto unit = static_cast<std::make_unsigned_t<fs::path::value_type>>(c);
        for (int shift = int(sizeof(c)*8)-4; shift >= 0; shift -= 4) raw += hex[(unit >> shift) & 15];
    }
    return {{"text",display_path(path)},{"native",raw}};
}
static fs::path decode_path(const Json& j) {
    auto raw = j.at("native").get<std::string>();
    constexpr size_t width = sizeof(fs::path::value_type)*2;
    if (raw.size() % width || raw.size() > 131072) throw std::runtime_error("Invalid native path");
    fs::path::string_type out;
    for (size_t i = 0; i < raw.size(); i += width) {
        unsigned unit = 0;
        for (size_t k = 0; k < width; ++k) {
            char c = raw[i+k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) throw std::runtime_error("Invalid path encoding");
            unit = unit*16 + (c <= '9' ? c-'0' : c-'a'+10);
        }
        if (!unit) throw std::runtime_error("NUL in path");
        out += static_cast<fs::path::value_type>(unit);
    }
    return fs::path(out);
}
static Json encode(const SavedState& s) {
    Json j = {{"schema",1},{"native_unit_bytes",sizeof(fs::path::value_type)},
        {"active_panel",s.activePanel},{"split",s.splitPercent},{"width",s.width},{"height",s.height}};
    j["panels"] = Json::array();
    j["bookmarks"] = Json::array();
    for (const auto& path : s.bookmarks) j["bookmarks"].push_back(encode_path(path));
    for (auto& p : s.panels) {
        Json panel = {{"active",p.active},{"tree_visible",p.treeVisible},{"tree_width",p.treeWidth},{"tabs",Json::array()}};
        for (auto& t : p.tabs) {
            Json tab = {{"path",encode_path(t.path)},{"focus",encode_path(t.focus)},{"anchor",encode_path(t.anchor)},
                {"sort",static_cast<int>(t.options.sort)},{"descending",t.options.descending},{"hidden",t.options.hidden},
                {"history_index",t.historyIndex},{"history",Json::array()},{"expanded",Json::array()}};
            for (auto& path : t.history) tab["history"].push_back(encode_path(path));
            for (auto& path : t.expanded) tab["expanded"].push_back(encode_path(path));
            panel["tabs"].push_back(tab);
        }
        j["panels"].push_back(panel);
    }
    return j;
}
StateLoad load_state(const fs::path& path) {
    StateLoad r;
    try {
        std::error_code ec;
        if (!fs::exists(path,ec)) { if (ec) r.error = {"state_read",ec.message()}; return r; }
        r.found = true;
        if (fs::file_size(path) > 8*1024*1024) throw std::runtime_error("Session file exceeds 8 MiB");
        std::ifstream f(path,std::ios::binary); if (!f) throw std::runtime_error("Cannot read session");
        auto j = Json::parse(f);
        if (j.at("schema") != 1 || j.at("native_unit_bytes") != sizeof(fs::path::value_type)) throw std::runtime_error("Unsupported session schema/platform; original file preserved");
        auto& s = r.state;
        if (j.contains("bookmarks")) {
            const auto& items = j.at("bookmarks");
            if (!items.is_array() || items.size() > 128) throw std::runtime_error("Invalid bookmark count");
            for (const auto& item : items) {
                auto value = decode_path(item);
                if (!value.is_absolute()) throw std::runtime_error("Bookmark is not absolute");
                s.bookmarks.push_back(std::move(value));
            }
        }
        s.activePanel = j.at("active_panel").get<int>(); s.splitPercent = j.at("split").get<int>();
        s.width = std::clamp(j.value("width",1200),640,7680); s.height = std::clamp(j.value("height",760),400,4320);
        const auto& panels = j.at("panels");
        if (!panels.is_array() || panels.size() != 2) throw std::runtime_error("Expected two panels");
        for (int i = 0; i < 2; ++i) {
            auto& p = s.panels[i]; const auto& src = panels[i];
            p.active = src.at("active").get<size_t>(); p.treeVisible = src.at("tree_visible").get<bool>(); p.treeWidth = src.at("tree_width").get<int>();
            if (!src.at("tabs").is_array() || src.at("tabs").empty() || src.at("tabs").size() > 32) throw std::runtime_error("Invalid tab count");
            for (auto& tj : src.at("tabs")) {
                SavedTab t; t.path = decode_path(tj.at("path"));
                if (!t.path.is_absolute()) throw std::runtime_error("Session location is not absolute");
                t.focus = decode_path(tj.at("focus")); t.anchor = decode_path(tj.at("anchor"));
                int sort = tj.at("sort").get<int>(); if (sort < 0 || sort > 3) throw std::runtime_error("Invalid sort field");
                t.options = {static_cast<Sort>(sort),tj.at("descending").get<bool>(),tj.at("hidden").get<bool>()};
                t.historyIndex = tj.at("history_index").get<size_t>();
                if (!tj.at("history").is_array() || tj.at("history").size() > 100 || !tj.at("expanded").is_array() || tj.at("expanded").size() > 256) throw std::runtime_error("Session history limit exceeded");
                for (auto& h : tj.at("history")) { auto v = decode_path(h); if (!v.is_absolute()) throw std::runtime_error("Invalid history path"); t.history.push_back(v); }
                for (auto& h : tj.at("expanded")) { auto v = decode_path(h); if (!v.is_absolute()) throw std::runtime_error("Invalid tree path"); t.expanded.push_back(v); }
                p.tabs.push_back(std::move(t));
            }
        }
    } catch (const std::exception& e) { r.error = {"state_read",e.what()}; }
    return r;
}
Error save_state(const fs::path& path, const SavedState& s) {
    auto temp = path; temp += ".tmp";
    try {
        auto existing = load_state(path);
        if (existing.error) return {"state_write","Existing session is invalid; refusing to overwrite: " + existing.error.message};
        fs::create_directories(path.parent_path());
        std::string body = encode(s).dump(2,' ',false,Json::error_handler_t::replace);
        if (body.size() > 8*1024*1024) return {"resource_limit","Session exceeds 8 MiB"};
        {
            std::ofstream f(temp,std::ios::binary | std::ios::trunc);
            if (!f || !(f << body << '\n') || !f.flush()) throw std::runtime_error("Cannot write temporary session");
        }
#ifdef _WIN32
        HANDLE h = CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush session");
        BOOL flushed = FlushFileBuffers(h); CloseHandle(h);
        if (!flushed) throw std::runtime_error("Session flush failed");
#else
        int fd = open(temp.c_str(),O_RDONLY); if (fd < 0) throw std::runtime_error("Cannot flush session");
        int flushed = fsync(fd); close(fd); if (flushed) throw std::runtime_error("Session flush failed");
#endif
        auto backup = path; backup += ".bak";
        if (existing.found) fs::copy_file(path,backup,fs::copy_options::overwrite_existing);
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace session");
#else
        fs::rename(temp,path);
#endif
        return {};
    } catch (const std::exception& e) { return {"state_write",e.what()}; }
}
fs::path default_state_path() {
#ifdef _WIN32
    if (auto p = _wgetenv(L"LOCALAPPDATA")) return fs::path(p)/L"aicmd"/L"session.json";
#elif defined(__APPLE__)
    if (auto p = std::getenv("HOME")) return fs::path(p)/"Library/Application Support/aicmd/session.json";
#else
    if (auto p = std::getenv("XDG_STATE_HOME")) return fs::path(p)/"aicmd/session.json";
    if (auto p = std::getenv("HOME")) return fs::path(p)/".local/state/aicmd/session.json";
#endif
    return fs::temp_directory_path()/"aicmd/session.json";
}
}
