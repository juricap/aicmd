#pragma once
#include <filesystem>
#include <string>
#include <stdexcept>
namespace ac {
namespace fs = std::filesystem;
// Remote paths retain POSIX syntax and are never passed to local filesystem APIs.
struct Location {
    fs::path local;
    std::string host, remote;
    Location() = default;
    Location(const fs::path& path);
    Location(const std::wstring& path) : Location(fs::path(path)) {}
    Location(const char* path) : Location(fs::u8path(path)) {}
    Location(const wchar_t* path) : Location(fs::path(path)) {}
    static Location ssh(std::string host, std::string path = "/");
    bool is_remote() const { return !host.empty(); }
    bool empty() const { return host.empty() && local.empty(); }
    bool is_absolute() const { return is_remote() || local.is_absolute(); }
    Location parent_path() const;
    Location root_path() const;
    fs::path filename() const;
    std::string u8string() const;
    fs::path::string_type native() const { return is_remote() ? fs::u8path(u8string()).native() : local.native(); }
    Location operator/(const fs::path& name) const;
    friend bool operator==(const Location& a,const Location& b) { return a.host == b.host && a.remote == b.remote && a.local == b.local; }
    friend bool operator!=(const Location& a,const Location& b) { return !(a == b); }
    friend bool operator<(const Location& a,const Location& b) { return a.host != b.host ? a.host < b.host : a.remote != b.remote ? a.remote < b.remote : a.local < b.local; }
};
Location absolute_path(const Location&, const Location&);
std::string display_path(const Location&);
bool valid_ssh_host(const std::string&);
}
