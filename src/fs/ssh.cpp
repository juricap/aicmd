#include "ssh.h"
#include "ssh_process.h"
#include "read_source.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace ac {
namespace {
struct Packet {
    std::string bytes; size_t pos = 0;
    Packet() = default;
    explicit Packet(unsigned type) { bytes += static_cast<char>(type); }
    void u32(uint32_t n) { for (int i = 24; i >= 0; i -= 8) bytes += static_cast<char>(n >> i); }
    void u64(uint64_t n) { u32(static_cast<uint32_t>(n >> 32)); u32(static_cast<uint32_t>(n)); }
    void str(const std::string& s) { u32(static_cast<uint32_t>(s.size())); bytes += s; }
    unsigned byte() { if (pos == bytes.size()) throw std::runtime_error("Truncated SFTP response"); return static_cast<unsigned char>(bytes[pos++]); }
    uint32_t get32() { uint32_t n = 0; for (int i = 0; i < 4; ++i) n = (n << 8)|byte(); return n; }
    uint64_t get64() { uint64_t hi = get32(); return (hi << 32)|get32(); }
    std::string str() { auto n = get32(); if (n > bytes.size()-pos) throw std::runtime_error("Invalid SFTP string"); auto s = bytes.substr(pos,n); pos += n; return s; }
};
struct Attr {
    uint64_t size = 0; uint32_t mode = 0, modified = 0, flags = 0;
    bool directory() const { return (mode & 0170000) == 0040000; }
    bool link() const { return (mode & 0170000) == 0120000; }
    bool regular() const { return (mode & 0170000) == 0100000; }
};
Attr attributes(Packet& p) {
    Attr a; a.flags = p.get32();
    if (a.flags & 1) a.size = p.get64();
    if (a.flags & 2) { p.get32(); p.get32(); }
    if (a.flags & 4) a.mode = p.get32();
    if (a.flags & 8) { p.get32(); a.modified = p.get32(); }
    if (a.flags & 0x80000000) { auto n = p.get32(); if (n > 1024) throw std::runtime_error("Too many SFTP attributes"); while (n--) { p.str(); p.str(); } }
    return a;
}
struct Sftp {
    SshPipe pipe; uint32_t next = 1;
    explicit Sftp(const std::string& host,const Cancel& cancel) : pipe(host) {
        SshDeadline deadline(pipe,cancel); Packet init(1); init.u32(3); send(init);
        auto version = receive(); if (version.byte() != 2 || version.get32() != 3) throw std::runtime_error("Server must support SFTP version 3");
    }
    void send(const Packet& p) { Packet size; size.u32(static_cast<uint32_t>(p.bytes.size())); pipe.write(size.bytes.data(),size.bytes.size()); pipe.write(p.bytes.data(),p.bytes.size()); }
    Packet receive() {
        Packet size; size.bytes.resize(4); pipe.read(size.bytes.data(),4); auto n = size.get32();
        if (n < 1 || n > 4*1024*1024) throw std::runtime_error("Invalid SFTP packet length");
        Packet p; p.bytes.resize(n); pipe.read(p.bytes.data(),n); return p;
    }
    Packet request(unsigned type) { Packet p(type); p.u32(next++); return p; }
    static void status(Packet& p,bool eof = false) {
        auto code = p.get32(); auto message = p.str();
        if (code && !(eof && code == 1)) throw std::runtime_error("SFTP "+std::to_string(code)+": "+safe_text(message));
    }
    Packet response(uint32_t id,unsigned wanted) {
        auto p = receive(); auto type = p.byte(); if (p.get32() != id) throw std::runtime_error("Unexpected SFTP request ID");
        if (type == 101) { status(p); if (wanted != 101) throw std::runtime_error("Missing SFTP response data"); }
        else if (type != wanted) throw std::runtime_error("Unexpected SFTP response type");
        return p;
    }
    Packet call(Packet p,unsigned wanted) { auto id = next-1; send(p); return response(id,wanted); }
    Attr stat(const std::string& path,bool follow = false) { auto p = request(follow ? 17 : 7); p.str(path); auto r = call(p,105); return attributes(r); }
    bool exists(const std::string& path) {
        auto p = request(7); p.str(path); auto id = next-1; send(p);
        auto r = receive(); auto type = r.byte(); if (r.get32() != id) throw std::runtime_error("Invalid SFTP stat reply");
        if (type == 105) { attributes(r); return true; }
        if (type == 101) { auto code = r.get32(); if (code == 2) return false; throw std::runtime_error("Cannot check destination: "+safe_text(r.str())); }
        throw std::runtime_error("Invalid SFTP stat reply type");
    }
    std::string open(const std::string& path,bool write) {
        auto p = request(3); p.str(path); p.u32(write ? 2|8|32 : 1); p.u32(write ? 4 : 0); if (write) p.u32(0600);
        return call(p,102).str();
    }
    void close(const std::string& handle) { auto p = request(4); p.str(handle); call(p,101); }
    void remove(const std::string& path) { auto p = request(13); p.str(path); call(p,101); }
    void rename(const std::string& from,const std::string& to) { auto p = request(18); p.str(from); p.str(to); call(p,101); }
    void mkdir(const std::string& path,uint32_t mode = 0700) { auto p = request(14); p.str(path); p.u32(4); p.u32(mode); call(p,101); }
    void rmdir(const std::string& path) { auto p = request(15); p.str(path); call(p,101); }
    std::string realpath(const std::string& path) { auto p = request(16); p.str(path); auto r = call(p,104); if (r.get32() != 1) throw std::runtime_error("Invalid SFTP canonical path"); return r.str(); }
    std::string read(const std::string& handle,uint64_t offset,size_t size) {
        std::map<uint32_t,std::pair<size_t,size_t>> pending;
        for (size_t at = 0; at < size; at += 32768) { auto p = request(5); auto id = next-1; auto n = std::min<size_t>(32768,size-at); p.str(handle); p.u64(offset+at); p.u32(static_cast<uint32_t>(n)); send(p); pending[id] = {at,n}; }
        std::string data(size,'\0');
        while (!pending.empty()) {
            auto r = receive(); auto type = r.byte(); auto id = r.get32(); auto it = pending.find(id);
            if (it == pending.end()) throw std::runtime_error("Unknown SFTP read reply");
            auto [at,n] = it->second; pending.erase(it);
            if (type == 101) { status(r); throw std::runtime_error("Source ended before its advertised size"); }
            if (type != 103) throw std::runtime_error("Expected SFTP file data");
            auto part = r.str(); if (part.empty() || part.size() > n) throw std::runtime_error("Invalid SFTP read length");
            std::memcpy(data.data()+at,part.data(),part.size());
            if (part.size() < n) { auto p = request(5); auto retry = next-1; p.str(handle); p.u64(offset+at+part.size()); p.u32(static_cast<uint32_t>(n-part.size())); send(p); pending[retry] = {at+part.size(),n-part.size()}; }
        }
        return data;
    }
    void write(const std::string& handle,uint64_t offset,const std::string& data) {
        std::set<uint32_t> pending;
        for (size_t at = 0; at < data.size(); at += 32768) { auto p = request(6); pending.insert(next-1); p.str(handle); p.u64(offset+at); p.str(data.substr(at,std::min<size_t>(32768,data.size()-at))); send(p); }
        while (!pending.empty()) { auto r = receive(); auto type = r.byte(); if (!pending.erase(r.get32()) || type != 101) throw std::runtime_error("Invalid SFTP write reply"); status(r); }
    }
    Listing list(const Location& location,bool dirs,const Cancel& cancel,size_t budget) {
        auto p = request(11); p.str(location.remote); auto handle = call(p,102).str(); Listing result;
        try {
            for (;;) {
                if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
                auto q = request(12); q.str(handle); auto id = next-1; send(q); auto r = receive(); auto type = r.byte();
                if (r.get32() != id) throw std::runtime_error("Invalid directory response ID");
                if (type == 101) { auto code = r.get32(); if (code == 1) break; throw std::runtime_error("SFTP listing failed: "+safe_text(r.str())); }
                if (type != 104) throw std::runtime_error("Invalid SFTP listing");
                auto count = r.get32(); if (count > 100000) throw std::runtime_error("Invalid SFTP entry count");
                while (count--) {
                    auto name = r.str(); r.str(); auto attr = attributes(r);
                    if (name == "." || name == "..") continue;
                    if (name.empty() || name.find('/') != std::string::npos || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid SFTP entry name");
                    Entry e; e.name = fs::u8path(name); e.id = result.entries.size()+1; e.directory = attr.directory(); e.link = attr.link(); e.hidden = name[0] == '.';
                    if (attr.flags & 1) e.size = attr.size;
                    if (attr.flags & 8) e.modified = attr.modified;
                    // Symlink metadata is resolved after the directory packet has been consumed.
                    if (dirs && !e.directory && !e.link) continue;
                    result.bytes += sizeof(Entry)+name.size()*4+128;
                    if (result.bytes > budget) throw std::runtime_error("Directory listing exceeds memory budget");
                    result.entries.push_back(std::move(e));
                }
            }
            close(handle);
            for (auto& e : result.entries) if (e.link) { try { e.directory = stat((location/e.name).remote,true).directory(); } catch (...) { if (cancel && cancel->load()) throw; } }
            if (dirs) result.entries.erase(std::remove_if(result.entries.begin(),result.entries.end(),[](const Entry& e){return !e.directory;}),result.entries.end());
            return result;
        } catch (...) { pipe.kill(); throw; }
    }
};
struct Connection { std::timed_mutex mutex; std::unique_ptr<Sftp> sftp; };
std::mutex poolMutex;
std::map<std::string,std::shared_ptr<Connection>> pool;
struct Access {
    std::shared_ptr<Connection> connection;
    std::unique_lock<std::timed_mutex> lock;
    std::unique_ptr<SshDeadline> deadline;
    int exceptions = std::uncaught_exceptions();
    Access(const std::string& host,const char* lane,const Cancel& cancel) {
        { std::lock_guard<std::mutex> guard(poolMutex); auto key = host+"/"+lane; auto& slot = pool[key]; if (!slot) { if (pool.size() > 48) { pool.erase(key); throw std::runtime_error("Too many SSH connections; restart to release idle hosts"); } slot = std::make_shared<Connection>(); } connection = slot; }
        lock = std::unique_lock<std::timed_mutex>(connection->mutex,std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (!connection->sftp) connection->sftp = std::make_unique<Sftp>(host,cancel);
        deadline = std::make_unique<SshDeadline>(connection->sftp->pipe,cancel);
    }
    ~Access() { deadline.reset(); if (std::uncaught_exceptions() > exceptions) connection->sftp.reset(); }
    Sftp* operator->() { return connection->sftp.get(); }
};
class Filesystems final : public Provider {
    std::shared_ptr<Provider> local = local_provider();
public:
    std::vector<Location> roots() override { return local->roots(); }
    Listing list(const Location& path,bool dirs,const Cancel& cancel,size_t budget) override {
        if (!path.is_remote()) return local->list(path,dirs,cancel,budget);
        try { Access access(path.host,"browse",cancel); return access->list(path,dirs,cancel,budget); }
        catch (const std::exception& e) { return {{},{cancel && cancel->load() ? "cancelled" : "ssh",e.what()}}; }
    }
};
}
std::shared_ptr<Provider> filesystem_provider() { return std::make_shared<Filesystems>(); }
namespace {
class RemoteRead final : public ReadSource {
    Cancel cancel;
    Sftp sftp;
    std::string handle;
    uint64_t length;
public:
    RemoteRead(const Location& location,const Cancel& token) : cancel(token),sftp(location.host,token) {
        SshDeadline deadline(sftp.pipe,cancel);
        auto attr = sftp.stat(location.remote,true);
        if (!attr.regular() || !(attr.flags & 1) || attr.size > INT64_MAX) throw std::runtime_error("Media source must be a regular, sized file");
        length = attr.size; handle = sftp.open(location.remote,false);
    }
    // Each source owns its connection: destruction interrupts it without affecting browsing/copies.
    uint64_t size() const override { return length; }
    std::string read_at(uint64_t offset,size_t count) override {
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (offset >= length) return {};
        SshDeadline deadline(sftp.pipe,cancel);
        return sftp.read(handle,offset,static_cast<size_t>(std::min<uint64_t>({count,512*1024,length-offset})));
    }
};
class LocalRead final : public ReadSource {
    std::ifstream input; uint64_t length; Cancel cancel;
public:
    LocalRead(const Location& file,const Cancel& token) : input(file.local,std::ios::binary),length(fs::file_size(file.local)),cancel(token) {
        if (!input || length > INT64_MAX) throw std::runtime_error("Cannot open media source");
    }
    uint64_t size() const override { return length; }
    std::string read_at(uint64_t offset,size_t count) override {
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (offset >= length) return {};
        std::string data(static_cast<size_t>(std::min<uint64_t>({count,512*1024,length-offset})),'\0');
        input.clear(); input.seekg(static_cast<std::streamoff>(offset)); input.read(data.data(),data.size());
        if (static_cast<size_t>(input.gcount()) != data.size()) throw std::runtime_error("Media source changed or read failed");
        return data;
    }
};
}
std::unique_ptr<ReadSource> open_read(const Location& file,const Cancel& cancel) {
    if (file.is_remote()) return std::make_unique<RemoteRead>(file,cancel);
    return std::make_unique<LocalRead>(file,cancel);
}
static void local_name(const fs::path& name);
fs::path stage_view(const Location& source,const Cancel& cancel,const Progress& progress) {
    if (!source.is_remote()) return source.local;
    struct Cached { Attr remote; fs::path file; fs::file_time_type modified; };
    static std::mutex cacheMutex; static std::map<Location,Cached> cache;
    Attr metadata;
    { Access access(source.host,"browse",cancel); metadata = access->stat(source.remote); }
    if (!metadata.regular() || !(metadata.flags & 1)) throw std::runtime_error("Viewer requires a regular file");
    if (metadata.size > 1024ull*1024*1024) throw std::runtime_error("Remote viewer staging is limited to 1 GiB; use F5 to download this file");
    {
        std::lock_guard<std::mutex> guard(cacheMutex); auto it = cache.find(source); std::error_code ec;
        if (it != cache.end() && (metadata.flags & 8) && it->second.remote.size == metadata.size && it->second.remote.modified == metadata.modified && fs::file_size(it->second.file,ec) == metadata.size && !ec && fs::last_write_time(it->second.file,ec) == it->second.modified && !ec) return it->second.file;
    }
    static std::atomic<uint64_t> sequence{0};
    auto root = fs::temp_directory_path()/"aicmd-view"; fs::create_directories(root);
    uint64_t used = 0; size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (++count > 10000) throw std::runtime_error("Viewer cache needs cleanup: "+display_path(root));
        if (entry.is_regular_file()) used += entry.file_size();
        if (used > 2ull*1024*1024*1024 || metadata.size > 2ull*1024*1024*1024-used) throw std::runtime_error("Viewer cache is full; close old views and clean "+display_path(root));
    }
    auto folder = root/(std::to_string(SshPipe::now())+"-"+std::to_string(++sequence));
    if (!fs::create_directory(folder)) throw std::runtime_error("Cannot create viewer staging directory");
    fs::permissions(folder,fs::perms::owner_all,fs::perm_options::replace);
    auto name = source.filename();
    try { local_name(name); } catch (...) { name = "remote-view"; }
    auto file = folder/name;
    try { copy_file(source,file,cancel,progress); }
    catch (...) { std::error_code ec; fs::remove(folder,ec); throw; }
    auto modified = fs::last_write_time(file);
    { std::lock_guard<std::mutex> guard(cacheMutex); if (cache.size() >= 128) cache.erase(cache.begin()); cache[source] = {metadata,file,modified}; }
    return file;
}
static void local_name(const fs::path& name) {
    if (name.empty() || name == "." || name == ".." || name.filename() != name) throw std::runtime_error("Remote filename cannot be represented on the destination filesystem");
#ifdef _WIN32
    auto n = name.native();
    if (n.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos || n.back() == L'.' || n.back() == L' ') throw std::runtime_error("Remote filename is not a valid Windows filename");
    for (auto c : n) if (c < 32) throw std::runtime_error("Remote filename contains a Windows control character");
    auto stem = n.substr(0,n.find(L'.')); for (auto& c : stem) if (c >= L'a' && c <= L'z') c -= L'a'-L'A';
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" || (stem.size() == 4 && (stem.substr(0,3) == L"COM" || stem.substr(0,3) == L"LPT") && stem[3] >= L'0' && stem[3] <= L'9')) throw std::runtime_error("Remote filename is a reserved Windows device name");
#endif
}
void copy_location(const Location& source,const Location& destination,const Cancel& cancel,const Progress& progress) {
    auto provider = filesystem_provider(); uint64_t completed = 0; size_t entries = 0;
    std::function<void(const Location&,const Location&,int)> visit = [&](const Location& from,const Location& to,int depth) {
        if ((cancel && cancel->load()) || depth > 64 || ++entries > 1000000) throw std::runtime_error("Copy cancelled or recursive copy limit reached");
        bool directory = false;
        if (from.is_remote()) { Access access(from.host,"browse",cancel); auto a = access->stat(from.remote); if (a.link()) throw std::runtime_error("Recursive copy does not follow symlinks"); directory = a.directory(); }
        else { auto a = fs::symlink_status(from.local); if (fs::is_symlink(a)) throw std::runtime_error("Recursive copy does not follow symlinks"); directory = fs::is_directory(a); }
        if (!directory) { uint64_t bytes = 0; copy_file(from,to,cancel,[&](uint64_t n){bytes = n; if (progress) progress(completed+n);}); completed += bytes; return; }
        if (to.is_remote()) { Access access(to.host,"write",cancel); access->mkdir(to.remote); }
        else if (!fs::create_directory(to.local)) throw std::runtime_error("Destination directory exists; merge/overwrite is not enabled");
        auto listing = provider->list(from,false,cancel,32*1024*1024); if (listing.error) throw std::runtime_error(listing.error.message);
        for (auto& e : listing.entries) {
            if (e.link) throw std::runtime_error("Copy stopped at symlink: "+display_path(e.name));
            if (!to.is_remote()) local_name(e.name);
            visit(from/e.name,to/e.name,depth+1);
        }
    };
    // Do not recursively copy a directory into itself or its descendants.
    Location canonicalSource = source, at = destination;
    if (source.host == destination.host) {
        if (source.is_remote()) { Access access(source.host,"browse",cancel); canonicalSource = Location::ssh(source.host,access->realpath(source.remote)); at = Location::ssh(destination.host,access->realpath(destination.parent_path().remote))/destination.filename(); }
        else { canonicalSource = fs::weakly_canonical(source.local); at = fs::weakly_canonical(destination.local); }
    }
    for (int depth = 0; depth < 1024; ++depth) { if (at == canonicalSource) throw std::runtime_error("Destination is inside source"); auto parent = at.parent_path(); if (parent == at || parent.empty()) break; at = parent; }
    if (source.is_remote() && !destination.is_remote()) local_name(source.filename());
    visit(source,destination,0);
}
void copy_file(const Location& source,const Location& destination,const Cancel& cancel,const Progress& progress) {
    if (source == destination) throw std::runtime_error("Source and destination are identical");
    bool exists = false;
    if (destination.is_remote()) { Access access(destination.host,"write",cancel); exists = access->exists(destination.remote); }
    else exists = fs::exists(fs::symlink_status(destination.local));
    if (exists) throw std::runtime_error("Destination exists; no overwrite performed");
    std::unique_ptr<Access> reader,writer;
    std::ifstream input;
    Attr before{}; uint64_t size = 0;
    fs::file_time_type localModified{};
    std::string readHandle,writeHandle;
    if (source.is_remote()) {
        reader = std::make_unique<Access>(source.host,"read",cancel); before = (*reader)->stat(source.remote);
        if (!before.regular() || !(before.flags & 1)) throw std::runtime_error("Copy requires a regular file; symlinks and special files are not followed");
        size = before.size; readHandle = (*reader)->open(source.remote,false);
    } else {
        if (!fs::is_regular_file(fs::symlink_status(source.local))) throw std::runtime_error("Copy requires a regular file");
        size = fs::file_size(source.local); input.open(source.local,std::ios::binary); if (!input) throw std::runtime_error("Cannot open source");
        localModified = fs::last_write_time(source.local);
    }
    static std::atomic<uint64_t> sequence{0};
    auto tempName = ".aicmd-part-"+std::to_string(SshPipe::now())+"-"+std::to_string(++sequence);
    auto temp = destination.parent_path()/fs::u8path(tempName);
#ifdef _WIN32
    HANDLE output = INVALID_HANDLE_VALUE;
#else
    int output = -1;
#endif
    bool created = false;
    try {
        if (destination.is_remote()) { writer = std::make_unique<Access>(destination.host,"write",cancel); writeHandle = (*writer)->open(temp.remote,true); created = true; }
        else {
#ifdef _WIN32
            output = CreateFileW(temp.local.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr); if (output == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create destination staging file");
#else
            output = ::open(temp.local.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); if (output < 0) throw std::runtime_error("Cannot create destination staging file");
#endif
            created = true;
        }
        for (uint64_t offset = 0; offset < size;) {
            if (cancel && cancel->load()) throw std::runtime_error("Copy cancelled");
            size_t n = static_cast<size_t>(std::min<uint64_t>(512*1024,size-offset)); std::string data;
            if (reader) data = (*reader)->read(readHandle,offset,n);
            else { data.resize(n); if (!input.read(data.data(),n)) throw std::runtime_error("Source changed or read failed"); }
            if (writer) (*writer)->write(writeHandle,offset,data);
            else {
                size_t at = 0;
                while (at < data.size()) {
#ifdef _WIN32
                    DWORD written = 0; if (!WriteFile(output,data.data()+at,static_cast<DWORD>(data.size()-at),&written,nullptr) || !written) throw std::runtime_error("Destination write failed");
#else
                    auto written = ::write(output,data.data()+at,data.size()-at); if (written <= 0) throw std::runtime_error("Destination write failed");
#endif
                    at += written;
                }
            }
            offset += n; if (progress) progress(offset);
        }
        if (reader) {
            auto after = (*reader)->stat(source.remote); if (after.size != before.size || after.modified != before.modified) throw std::runtime_error("Remote source changed during copy");
            (*reader)->close(readHandle); readHandle.clear();
        }
        else if (fs::file_size(source.local) != size || fs::last_write_time(source.local) != localModified) throw std::runtime_error("Local source changed during copy");
        if (writer) { (*writer)->close(writeHandle); writeHandle.clear(); (*writer)->rename(temp.remote,destination.remote); }
        else {
#ifdef _WIN32
            if (!FlushFileBuffers(output)) throw std::runtime_error("Cannot flush destination");
            CloseHandle(output); output = INVALID_HANDLE_VALUE;
            if (!MoveFileExW(temp.local.c_str(),destination.local.c_str(),MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Destination exists or cannot be published; no overwrite performed");
#else
            if (fsync(output)) throw std::runtime_error("Cannot flush destination");
            ::close(output); output = -1;
            if (::link(temp.local.c_str(),destination.local.c_str())) throw std::runtime_error("Destination exists or cannot be published; no overwrite performed");
            ::unlink(temp.local.c_str());
#endif
        }
        created = false;
    } catch (...) {
#ifdef _WIN32
        if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
#else
        if (output >= 0) ::close(output);
#endif
        if (created && !destination.is_remote()) { std::error_code ec; fs::remove(temp.local,ec); }
        if (created && writer) { try { if (!writeHandle.empty()) (*writer)->close(writeHandle); (*writer)->remove(temp.remote); } catch (...) {} }
        if (reader) reader->connection->sftp->pipe.kill();
        if (writer) writer->connection->sftp->pipe.kill();
        throw;
    }
}
void validate_name(const Location& parent,const fs::path& name) {
    auto text = name.u8string();
    if (text.empty() || text == "." || text == ".." || text.find('/') != std::string::npos || text.find('\0') != std::string::npos) throw std::runtime_error("Name must be a single, non-empty path component");
    if (text.size() > 255) throw std::runtime_error("Name is too long");
    if (!parent.is_remote()) local_name(name);
}
void make_directory(const Location& path,const Cancel& cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
    if (path.is_remote()) {
        Access access(path.host,"write",cancel);
        if (access->exists(path.remote)) throw std::runtime_error("An entry with that name already exists");
        access->mkdir(path.remote,0777); return;
    }
    std::error_code ec;
    if (!fs::create_directory(path.local,ec)) throw std::runtime_error(ec ? "Cannot create folder: "+ec.message() : "An entry with that name already exists");
}
static std::string lower_ascii(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c-'A'+'a'); return s; }
bool rename_location(const Location& from,const Location& to,const Cancel& cancel) {
    if (from.host != to.host) return false;
    if (from == to) throw std::runtime_error("Source and destination are identical");
    if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
    if (from.is_remote()) {
        Access access(from.host,"write",cancel);
        // Case-only renames may stat as existing on case-insensitive servers; OpenSSH RENAME itself does not replace.
        if (lower_ascii(from.remote) != lower_ascii(to.remote) && access->exists(to.remote)) throw std::runtime_error("Destination exists; no overwrite performed");
        access->rename(from.remote,to.remote); return true;
    }
#ifdef _WIN32
    // Without MOVEFILE_REPLACE_EXISTING an existing destination fails. Case-only renames succeed.
    if (MoveFileExW(from.local.c_str(),to.local.c_str(),0)) return true;
    auto error = GetLastError();
    if (error == ERROR_NOT_SAME_DEVICE) return false;
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) throw std::runtime_error("Destination exists; no overwrite performed");
    throw std::runtime_error("Rename failed: "+std::system_category().message(static_cast<int>(error)));
#else
    int result = -1;
#if defined(__linux__) && defined(RENAME_NOREPLACE)
    result = ::renameat2(AT_FDCWD,from.local.c_str(),AT_FDCWD,to.local.c_str(),RENAME_NOREPLACE);
#elif defined(__APPLE__)
    result = ::renamex_np(from.local.c_str(),to.local.c_str(),RENAME_EXCL);
#else
    if (fs::exists(fs::symlink_status(to.local))) errno = EEXIST; else result = ::rename(from.local.c_str(),to.local.c_str());
#endif
    if (!result) return true;
    if (errno == EXDEV) return false;
    if (errno == EEXIST || errno == ENOTEMPTY) throw std::runtime_error("Destination exists; no overwrite performed");
    throw std::runtime_error("Rename failed: "+std::generic_category().message(errno));
#endif
}
void delete_location(const Location& target,bool trash,const Cancel& cancel,const std::function<void()>& removed) {
    if (target.empty() || target.parent_path() == target || target.filename().empty()) throw std::runtime_error("Refusing to delete a filesystem root");
    size_t entries = 0;
    auto tick = [&]{ if (cancel && cancel->load()) throw std::runtime_error("Delete cancelled"); if (++entries > 10000000) throw std::runtime_error("Delete entry limit reached"); };
    if (target.is_remote()) {
        if (trash) throw std::runtime_error("Remote servers have no Recycle Bin; use permanent delete");
        Access access(target.host,"write",cancel);
        std::function<void(const std::string&,int)> visit = [&](const std::string& path,int depth) {
            tick(); if (depth > 256) throw std::runtime_error("Delete depth limit reached");
            auto a = access->stat(path); // lstat: links are removed, never followed
            if (a.directory()) {
                auto listing = access->list(Location::ssh(target.host,path),false,cancel,64*1024*1024);
                if (listing.error) throw std::runtime_error(listing.error.message);
                for (const auto& e : listing.entries) visit(path+"/"+e.name.u8string(),depth+1);
                access->rmdir(path);
            } else access->remove(path);
            if (removed) removed();
        };
        visit(target.remote,0); return;
    }
#ifdef _WIN32
    if (trash) {
        tick();
        auto full = fs::absolute(target.local);
        auto root = full.root_path().native();
        auto type = GetDriveTypeW(root.c_str());
        if (root.rfind(L"\\\\",0) == 0 || type == DRIVE_REMOTE || type == DRIVE_REMOVABLE) throw std::runtime_error("Recycle Bin is unavailable on network or removable drives; use Shift+F8 for permanent delete");
        if (full.native().size() >= MAX_PATH) throw std::runtime_error("Path is too long for the Recycle Bin; use Shift+F8 for permanent delete");
        if (!fs::exists(fs::symlink_status(full))) throw std::runtime_error("Entry no longer exists");
        std::wstring from = full.native(); from.push_back(L'\0'); from.push_back(L'\0');
        SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT|FOF_WANTNUKEWARNING;
        int rc = SHFileOperationW(&op);
        if (rc || op.fAnyOperationsAborted) throw std::runtime_error("Recycle Bin operation failed (code "+std::to_string(rc)+")");
        if (removed) removed();
        return;
    }
#else
    if (trash) throw std::runtime_error("Trash is not implemented on this platform; use permanent delete");
#endif
    std::function<void(const fs::path&,int)> visit = [&](const fs::path& path,int depth) {
        tick(); if (depth > 256) throw std::runtime_error("Delete depth limit reached");
        auto status = fs::symlink_status(path);
        bool recurse = fs::is_directory(status) && !fs::is_symlink(status);
#ifdef _WIN32
        auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Cannot read attributes: "+display_path(path));
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) recurse = false; // junctions and links: remove the link only
        if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(),attributes & ~FILE_ATTRIBUTE_READONLY);
#endif
        if (recurse) for (const auto& child : fs::directory_iterator(path)) visit(child.path(),depth+1);
#ifdef _WIN32
        BOOL ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
        if (!ok) throw std::runtime_error("Cannot delete "+display_path(path)+": "+std::system_category().message(static_cast<int>(GetLastError())));
#else
        if (::remove(path.c_str())) throw std::runtime_error("Cannot delete "+display_path(path)+": "+std::generic_category().message(errno));
#endif
        if (removed) removed();
    };
    visit(target.local,0);
}
void move_location(const Location& source,const Location& destination,const Cancel& cancel,const Progress& progress) {
    if (source.host == destination.host) {
        Location a = source, b = destination;
        if (!source.is_remote()) { a = fs::weakly_canonical(source.local); b = fs::weakly_canonical(destination.local); }
        for (auto at = b.parent_path(); ; at = at.parent_path()) {
            if (at == a) throw std::runtime_error("Destination is inside source");
            if (at.empty() || at.parent_path() == at) break;
        }
    }
    if (rename_location(source,destination,cancel)) return;
    copy_location(source,destination,cancel,progress);
    // The source is removed only after the complete copy succeeded.
    delete_location(source,false,cancel);
}
}
