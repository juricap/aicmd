#include "core.h"
#include <cctype>
namespace ac {
bool valid_ssh_host(const std::string& h) {
    if (h.empty() || h.size() > 255 || h[0] == '-') return false;
    for (unsigned char c : h) if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    return true;
}
Location Location::ssh(std::string h,std::string p) {
    if (!valid_ssh_host(h) || p.empty() || p[0] != '/' || p.find('\0') != std::string::npos) throw std::runtime_error("Invalid SSH location");
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    Location l; l.host = std::move(h); l.remote = std::move(p); return l;
}
Location::Location(const fs::path& p) {
    auto s = p.u8string();
    if (s.rfind("ssh://",0) != 0) { local = p; return; }
    auto slash = s.find('/',6);
    *this = ssh(s.substr(6,slash == std::string::npos ? slash : slash-6),slash == std::string::npos ? "/" : s.substr(slash));
}
std::string Location::u8string() const { return is_remote() ? "ssh://"+host+remote : local.u8string(); }
Location Location::parent_path() const {
    if (!is_remote()) return local.parent_path();
    auto end = remote.find_last_of('/'); return ssh(host,end ? remote.substr(0,end) : "/");
}
Location Location::root_path() const { return is_remote() ? ssh(host) : Location(local.root_path()); }
fs::path Location::filename() const { return is_remote() ? fs::u8path(remote == "/" ? "" : remote.substr(remote.find_last_of('/')+1)) : local.filename(); }
Location Location::operator/(const fs::path& name) const {
    if (!is_remote()) return local/name;
    auto n = name.u8string();
    if (n.find('\0') != std::string::npos) throw std::runtime_error("NUL in remote path");
    return ssh(host,!n.empty() && n[0] == '/' ? n : remote+(remote == "/" ? "" : "/")+n);
}
Location absolute_path(const Location& p,const Location& base) {
    if (p.is_remote()) return p;
    if (p.empty()) return base;
    if (base.is_remote()) {
#ifdef _WIN32
        if (p.local.has_root_name()) return absolute_path(p.local,fs::current_path());
#endif
        if (p.local == "..") return base.parent_path();
        if (p.local == ".") return base;
        return base/p.local;
    }
    return absolute_path(p.local,base.local);
}
std::string display_path(const Location& p) { return p.is_remote() ? safe_text(p.u8string()) : display_path(p.local); }
}
