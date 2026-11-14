#include "core/core.h"
#include <algorithm>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

namespace ac {
std::string safe_text(std::string s) {
    std::string out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c < 32 || c == 127) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}
std::string display_path(const fs::path& p) {
    try { return safe_text(p.u8string()); } catch (...) { return "[unrepresentable path]"; }
}
// Iterative glob matcher: '*' and '?' plus semicolon-separated masks. No regex backtracking.
bool matches_pattern(const fs::path& name, const std::string& pattern) {
    auto text = name.native();
    auto next = [](const fs::path::string_type& s,size_t i) {
        if (i >= s.size()) return i;
#ifdef _WIN32
        unsigned c = s[i++];
        if (c >= 0xd800 && c <= 0xdbff && i < s.size() && unsigned(s[i]) >= 0xdc00 && unsigned(s[i]) <= 0xdfff) ++i;
#else
        ++i; while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) ++i;
#endif
        return i;
    };
    auto lower = [](fs::path::value_type c) { return c >= 'A' && c <= 'Z' ? c+('a'-'A') : c; };
    size_t begin = 0;
    while (begin <= pattern.size()) {
        auto end = pattern.find(';',begin); if (end == std::string::npos) end = pattern.size();
        auto mask = fs::u8path(pattern.substr(begin,end-begin)).native();
        if (mask == fs::path("*.*").native()) mask = fs::path("*").native();
        size_t p = 0, t = 0, star = std::string::npos, retry = 0;
        bool failed = false;
        while (t < text.size()) {
            if (p < mask.size() && mask[p] == '?') { ++p; t = next(text,t); }
            else if (p < mask.size() && mask[p] == '*') { star = p++; retry = t; }
            else if (p < mask.size() && lower(mask[p]) == lower(text[t])) { ++p; ++t; }
            else if (star != std::string::npos && retry < text.size()) { p = star+1; retry = next(text,retry); t = retry; }
            else { failed = true; break; }
        }
        while (p < mask.size() && mask[p] == '*') ++p;
        if (!failed && !mask.empty() && p == mask.size()) return true;
        if (end == pattern.size()) break;
        begin = end+1;
    }
    return false;
}
fs::path absolute_path(const fs::path& p, const fs::path& base) {
    if (p.empty()) return base;
    // Root-relative and drive-relative Windows input must not inherit process CWD.
#ifdef _WIN32
    if (p.has_root_name() && !p.has_root_directory()) {
        if (p.root_name() != base.root_name()) return (p.root_name() / L"\\" / p.relative_path()).lexically_normal();
        return (base / p.relative_path()).lexically_normal();
    }
    if (!p.has_root_name() && p.has_root_directory()) return (base.root_name() / p).lexically_normal();
#endif
    auto result = (p.is_absolute() ? p : base / p).lexically_normal();
    if (result != result.root_path() && result.filename().empty()) result = result.parent_path();
    return result;
}
void sort_entries(std::vector<Entry>& es, const Options& o) {
    auto compare = [](const fs::path& a,const fs::path& b) {
#ifdef _WIN32
        int n = CompareStringOrdinal(a.c_str(),static_cast<int>(a.native().size()),b.c_str(),static_cast<int>(b.native().size()),TRUE);
        return n ? n-CSTR_EQUAL : a.compare(b);
#else
        auto left = a.native(), right = b.native();
        for (auto& ch : left) if (ch >= 'A' && ch <= 'Z') ch += 'a'-'A';
        for (auto& ch : right) if (ch >= 'A' && ch <= 'Z') ch += 'a'-'A';
        return left.compare(right);
#endif
    };
    std::sort(es.begin(), es.end(), [&](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        int cmp = 0;
        if (o.sort == Sort::Size) cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
        if (o.sort == Sort::Modified) cmp = a.modified < b.modified ? -1 : a.modified > b.modified ? 1 : 0;
        if (o.sort == Sort::Extension) cmp = compare(a.name.extension(),b.name.extension());
        if (!cmp) cmp = compare(a.name,b.name);
        if (!cmp) cmp = a.name.compare(b.name); // Identity remains exact, even for case-only names.
        return o.descending ? cmp > 0 : cmp < 0;
    });
}
static Error error_of(int code) {
#ifdef _WIN32
    const char* kind = code == ERROR_ACCESS_DENIED ? "access_denied" :
        (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? "not_found" : "unavailable";
    return {kind, std::system_category().message(code), code};
#else
    return {code == EACCES ? "access_denied" : code == ENOENT ? "not_found" : code == ENOTDIR ? "invalid_path" : "unavailable", std::system_category().message(code), code};
#endif
}
class Local final : public Provider {
public:
    std::vector<Location> roots() override {
#ifdef _WIN32
        std::vector<Location> out;
        DWORD bits = GetLogicalDrives();
        for (int i = 0; i < 26; ++i) if (bits & (1u << i)) { std::wstring s = L"A:\\"; s[0] += static_cast<wchar_t>(i); out.emplace_back(s); }
        return out;
#else
        return {fs::path("/")};
#endif
    }
    Listing list(const Location& location, bool dirs, const Cancel& cancel, size_t budget) override {
        Listing out;
        out.error = enumerate(location,cancel,[&](Entry&& e) {
            if (dirs && !e.directory) return true;
            out.bytes += sizeof(Entry) * 2 + e.name.native().size() * sizeof(fs::path::value_type) * 2;
            if (out.bytes > budget) { out.error = {"resource_limit", "Directory exceeds the listing memory budget"}; return false; }
            e.id = out.entries.size() + 1;
            out.entries.push_back(std::move(e));
            return true;
        },"browse");
        if (!out.error && out.bytes > budget) out.error = {"resource_limit", "Directory exceeds the listing memory budget"};
        if (out.error) { out.entries.clear(); out.bytes = 0; }
        return out;
    }
    Error enumerate(const Location& location, const Cancel& cancel, const std::function<bool(Entry&&)>& visit, const char*) override {
        if (location.is_remote()) return {"unsupported","Remote provider unavailable"};
        const auto& path = location.local;
        Error error;
#ifdef _WIN32
        auto native = path.native();
        if (native.rfind(L"\\\\?\\", 0) != 0) {
            if (native.rfind(L"\\\\", 0) == 0) native = L"\\\\?\\UNC\\" + native.substr(2);
            else native = L"\\\\?\\" + native;
        }
        DWORD attr = GetFileAttributesW(native.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return error_of(GetLastError());
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return {"invalid_path", "Location is not a directory"};
        if (native.back() != L'\\') native += L'\\';
        WIN32_FIND_DATAW data{};
        HANDLE h = FindFirstFileExW((native + L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            return err == ERROR_FILE_NOT_FOUND ? Error{} : error_of(err);
        }
        bool stopped = false;
        do {
            if (cancel && cancel->load()) { error = {"cancelled", "Cancelled"}; break; }
            if (!wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L"..")) continue;
            Entry e; e.name = data.cFileName;
            e.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            e.hidden = (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            e.system = (data.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) != 0;
            e.readOnly = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
            e.archive = (data.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE) != 0;
            if (!e.directory) e.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            uint64_t ticks = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            e.modified = static_cast<int64_t>(ticks / 10000000) - 11644473600LL;
            if (!visit(std::move(e))) { stopped = true; break; }
        } while (FindNextFileW(h, &data));
        DWORD err = GetLastError();
        FindClose(h);
        if (!error && !stopped && err != ERROR_NO_MORE_FILES) error = error_of(err);
#else
        std::error_code ec;
        fs::directory_iterator it(path, ec), end;
        if (ec) return error_of(ec.value());
        while (it != end) {
            if (cancel && cancel->load()) { error = {"cancelled", "Cancelled"}; break; }
            Entry e; e.name = it->path().filename();
            e.hidden = !e.name.native().empty() && e.name.native()[0] == '.';
            e.link = it->is_symlink(ec); ec.clear();
            e.directory = it->is_directory(ec); ec.clear();
            if (!e.directory) { auto n = it->file_size(ec); if (!ec) e.size = n; ec.clear(); }
            auto t = it->last_write_time(ec);
            if (!ec) e.modified = std::chrono::duration_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now().time_since_epoch()).count();
            ec.clear();
            if (!visit(std::move(e))) break;
            it.increment(ec);
            if (ec) { error = error_of(ec.value()); break; }
        }
#endif
        return error;
    }
};
Error Provider::enumerate(const Location& location, const Cancel& cancel, const std::function<bool(Entry&&)>& visit, const char*) {
    auto listing = list(location,false,cancel,256*1024*1024);
    if (listing.error) return listing.error;
    for (auto& e : listing.entries) if (!visit(std::move(e))) break;
    return {};
}
std::shared_ptr<Provider> local_provider() { return std::make_shared<Local>(); }
}
