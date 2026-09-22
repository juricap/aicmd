#include "ssh.h"
#include <fstream>
#include <algorithm>
#include <cstdlib>
namespace ac {
fs::path ssh_config_path() {
#ifdef _WIN32
    if (auto home = _wgetenv(L"USERPROFILE")) return fs::path(home)/L".ssh/config";
#else
    if (auto home = std::getenv("HOME")) return fs::path(home)/".ssh/config";
#endif
    return {};
}
static std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> out; std::string part; bool quote = false;
    for (char c : line) {
        if (c == '"') { quote = !quote; continue; }
        if (!quote && c == '#') break;
        if (!quote && (c == ' ' || c == '\t' || c == '\r' || c == '=')) { if (!part.empty()) { out.push_back(part); part.clear(); } }
        else part += c;
    }
    if (!part.empty()) out.push_back(part);
    return out;
}
std::vector<std::string> ssh_hosts(const fs::path& config) {
    std::vector<std::string> result; std::set<fs::path> visited;
    std::function<void(fs::path,int)> read = [&](fs::path path,int depth) {
        if (depth > 8 || visited.size() >= 128 || !visited.insert(path.lexically_normal()).second) return;
        std::ifstream input(path); std::string line; size_t bytes = 0;
        while (std::getline(input,line) && (bytes += line.size()) < 1024*1024 && result.size() < 512) {
            auto parts = tokens(line); if (parts.empty()) continue;
            auto key = parts[0]; for (auto& c : key) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
            for (size_t i = 1; i < parts.size(); ++i) {
                if (key == "host" && valid_ssh_host(parts[i]) && std::find(result.begin(),result.end(),parts[i]) == result.end()) result.push_back(parts[i]);
                if (key == "include") {
                    auto name = parts[i]; fs::path include;
                    if (name.rfind("~/",0) == 0) include = config.parent_path().parent_path()/fs::u8path(name.substr(2));
                    else { include = fs::u8path(name); if (!include.is_absolute()) include = config.parent_path()/include; }
                    if (include.filename().u8string().find_first_of("*?") == std::string::npos) read(include,depth+1);
                    else {
                        std::error_code ec; std::vector<fs::path> paths;
                        for (fs::directory_iterator it(include.parent_path(),ec),end; !ec && it != end; it.increment(ec))
                            if (matches_pattern(it->path().filename(),include.filename().u8string())) paths.push_back(it->path());
                        std::sort(paths.begin(),paths.end()); for (const auto& p : paths) read(p,depth+1);
                    }
                }
            }
        }
    };
    if (!config.empty()) read(config,0);
    return result;
}
}
