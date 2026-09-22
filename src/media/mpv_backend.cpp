#include "backend.h"
#include "fs/read_source.h"
#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <list>

namespace av {
struct Backend::Impl {
    HMODULE dll{}; mpv_handle* mpv{}; Playback playback;
    uint64_t generation = 0, serial = 1;
    std::atomic<uint64_t> streamGeneration{0};
    int64_t startedEntry = -1;
    std::map<uint64_t,uint64_t> pending;
    std::string expected;
    struct Shot { uint64_t request,generation; int rotation; };
    std::map<uint64_t,Shot> shots;
    std::mutex mutex;
    std::vector<ac::Cancel> streams;
    std::atomic<uint64_t> bytes{0};
#define MPV_FN(name) decltype(&mpv_##name) name{}
    MPV_FN(create); MPV_FN(initialize); MPV_FN(terminate_destroy); MPV_FN(set_option_string);
    MPV_FN(command_async); MPV_FN(wait_event); MPV_FN(get_property); MPV_FN(free);
    MPV_FN(error_string); MPV_FN(stream_cb_add_ro); MPV_FN(client_api_version);
#undef MPV_FN
    ~Impl() { cancel(); if (mpv && terminate_destroy) terminate_destroy(mpv); if (dll) FreeLibrary(dll); }
    void cancel() { std::lock_guard<std::mutex> lock(mutex); for (auto& c : streams) c->store(true); }
    void check(int code) { if (code < 0) throw std::runtime_error(error_string(code)); }
    void option(const char* key,const char* value) { check(set_option_string(mpv,key,value)); }
    std::string string(const char* name) { char* value = nullptr; if (get_property(mpv,name,MPV_FORMAT_STRING,&value) < 0) return {}; std::string out(value); free(value); return out; }
    double number(const char* name) { double n = 0; get_property(mpv,name,MPV_FORMAT_DOUBLE,&n); return n; }
    bool flag(const char* name) { int n = 0; get_property(mpv,name,MPV_FORMAT_FLAG,&n); return n != 0; }
    void send(const std::vector<std::string>& args,uint64_t id = 0) {
        if (pending.size() >= 64) throw std::runtime_error("Playback command queue is full; retry after state update");
        if (!id) id = serial++;
        std::vector<const char*> argv; for (auto& a : args) argv.push_back(a.c_str()); argv.push_back(nullptr);
        check(command_async(mpv,id,argv.data()));
        pending[id] = generation;
    }
    struct Stream {
        Impl* owner; ac::Cancel token; std::unique_ptr<ac::ReadSource> source; uint64_t offset = 0, generation = 0;
        // A bounded LRU of 512 KiB blocks; random tail reads never fill preceding holes.
        std::list<std::pair<uint64_t,std::string>> blocks;
        ~Stream() { std::lock_guard<std::mutex> lock(owner->mutex); auto& all = owner->streams; all.erase(std::remove(all.begin(),all.end(),token),all.end()); }
    };
    static int open(void* raw,char* uri,mpv_stream_cb_info* info) {
        auto self = static_cast<Impl*>(raw);
        auto stream = std::make_unique<Stream>(); stream->owner = self; stream->token = std::make_shared<std::atomic<bool>>(false);
        try {
            std::string path(uri); if (path.rfind("aicmd://",0) != 0) return MPV_ERROR_LOADING_FAILED;
            auto slash = path.find('/',8); if (slash == std::string::npos) return MPV_ERROR_LOADING_FAILED;
            stream->generation = std::stoull(path.substr(8,slash-8));
            { std::lock_guard<std::mutex> lock(self->mutex); if (self->streamGeneration != stream->generation) return MPV_ERROR_LOADING_FAILED; self->streams.push_back(stream->token); }
            auto source = path.substr(slash+1); if (source.rfind("ssh://",0) != 0) return MPV_ERROR_LOADING_FAILED;
            stream->source = ac::open_read(ac::Location(ac::fs::u8path(source)),stream->token);
            info->cookie = stream.release();
            info->read_fn = [](void* raw,char* output,uint64_t count)->int64_t {
                auto& s = *static_cast<Stream*>(raw);
                try {
                    if (s.token->load()) return -1;
                    if (s.offset >= s.source->size()) return 0;
                    constexpr uint64_t blockSize = 512*1024;
                    auto base = s.offset/blockSize*blockSize;
                    auto found = std::find_if(s.blocks.begin(),s.blocks.end(),[&](const auto& block){return block.first == base;});
                    if (found == s.blocks.end()) {
                        auto data = s.source->read_at(base,blockSize); if (s.owner->streamGeneration == s.generation) s.owner->bytes += data.size();
                        s.blocks.emplace_front(base,std::move(data)); if (s.blocks.size() > 128) s.blocks.pop_back();
                    } else s.blocks.splice(s.blocks.begin(),s.blocks,found);
                    auto& data = s.blocks.front().second; auto at = static_cast<size_t>(s.offset-base);
                    if (at >= data.size()) return -1;
                    auto length = static_cast<size_t>(std::min<uint64_t>(count,data.size()-at));
                    std::memcpy(output,data.data()+at,length); s.offset += length; return length;
                } catch (...) { return -1; }
            };
            info->seek_fn = [](void* raw,int64_t at)->int64_t { auto& s = *static_cast<Stream*>(raw); if (s.token->load() || at < 0 || static_cast<uint64_t>(at) > s.source->size()) return MPV_ERROR_GENERIC; s.offset = at; return at; };
            info->size_fn = [](void* raw)->int64_t {return static_cast<int64_t>(static_cast<Stream*>(raw)->source->size());};
            info->cancel_fn = [](void* raw) {static_cast<Stream*>(raw)->token->store(true);};
            info->close_fn = [](void* raw) {delete static_cast<Stream*>(raw);};
            return 0;
        } catch (...) { return MPV_ERROR_LOADING_FAILED; }
    }
};
Backend::Backend() : impl(std::make_unique<Impl>()) {}
Backend::~Backend() = default;
void Backend::initialize(uintptr_t window) {
    auto& b = *impl;
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
    ac::fs::path dll = ac::fs::path(module).parent_path()/L"libmpv-2.dll";
    if (auto configured = _wgetenv(L"AIVIEW_MPV_DLL")) dll = ac::fs::absolute(configured);
    b.dll = LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!b.dll) throw std::runtime_error("libmpv-2.dll is missing or cannot load. Run scripts/setup-aiview.ps1, or set AIVIEW_MPV_DLL.");
#define LOAD(name) b.name = reinterpret_cast<decltype(b.name)>(GetProcAddress(b.dll,"mpv_" #name)); if (!b.name) throw std::runtime_error("Missing libmpv API: " #name)
    LOAD(create); LOAD(initialize); LOAD(terminate_destroy); LOAD(set_option_string); LOAD(command_async);
    LOAD(wait_event); LOAD(get_property); LOAD(free); LOAD(error_string); LOAD(stream_cb_add_ro); LOAD(client_api_version);
#undef LOAD
    auto api = b.client_api_version();
    if ((api >> 16) != 2 || api < MPV_CLIENT_API_VERSION) throw std::runtime_error("Compatible libmpv client API 2.5+ required; use the pinned setup runtime");
    b.mpv = b.create(); if (!b.mpv) throw std::runtime_error("Cannot create libmpv");
    b.option("config","no"); b.option("load-scripts","no"); b.option("input-default-bindings","no"); b.option("input-vo-keyboard","no");
    b.option("osc","no"); b.option("terminal","no"); b.option("idle","yes"); b.option("keep-open","always");
    b.option("image-display-duration","inf"); b.option("audio-display","no"); b.option("hwdec","auto-safe");
    b.option("wid",std::to_string(window).c_str()); b.option("vo","gpu"); b.option("gpu-api","d3d11");
    b.option("force-window","yes"); b.option("demuxer-max-bytes","32MiB"); b.option("demuxer-max-back-bytes","8MiB");
    b.option("cache-secs","5"); b.option("sub-auto","no"); b.option("audio-file-auto","no");
    // Capture source pixels, including still-image alpha, rather than GPU checkerboard compositing.
    b.option("screenshot-sw","yes");
    b.check(b.stream_cb_add_ro(b.mpv,"aicmd",&b,Impl::open)); b.check(b.initialize(b.mpv));
    b.playback.version = b.string("mpv-version");
}
void Backend::cancel() {impl->streamGeneration = 0; impl->cancel();}
void Backend::load(const Item& item,uint64_t generation) {
    auto& b = *impl; b.streamGeneration = 0; b.cancel(); b.generation = generation; b.streamGeneration = generation; b.bytes = 0;
    auto version = b.playback.version; b.playback = {}; b.playback.version = version;
    b.expected = item.location.is_remote() ? "aicmd://"+std::to_string(generation)+"/"+item.location.u8string() : item.location.u8string();
    b.send({"set","pause","no"}); b.send({"set","video-zoom","0"});
    b.send({"loadfile",b.expected,"replace"});
}
void Backend::command(const std::vector<std::string>& args) {impl->send(args);}
void Backend::capture(uint64_t request,uint64_t generation) {
    auto& b = *impl;
    if (!b.playback.ready || !b.playback.width) throw std::runtime_error("No decoded image/frame is ready");
    if (!b.shots.empty()) throw std::runtime_error("Frame capture already pending");
    auto id = b.serial++; b.shots[id] = {request,generation,static_cast<int>(b.number("video-params/rotate"))};
    try { b.send({"screenshot-raw","video","bgra"},id); } catch (...) { b.shots.erase(id); throw; }
}
const Playback& Backend::state() const {return impl->playback;}
void Backend::poll(const std::function<void(uint64_t,uint64_t,Pixels,std::string)>& captured) {
    auto& b = *impl;
    for (int i = 0; i < 256; ++i) {
        auto event = b.wait_event(b.mpv,0); if (!event || event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_START_FILE) b.startedEntry = static_cast<mpv_event_start_file*>(event->data)->playlist_entry_id;
        auto currentEntry = [&](int64_t id) {int64_t current = -1; return b.get_property(b.mpv,"playlist/0/id",MPV_FORMAT_INT64,&current) >= 0 && current == id && b.string("playlist/0/filename") == b.expected;};
        if (event->event_id == MPV_EVENT_FILE_LOADED && currentEntry(b.startedEntry)) { b.playback.ready = true; b.playback.error.clear(); }
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto end = static_cast<mpv_event_end_file*>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR && currentEntry(end->playlist_entry_id)) {b.playback.ready = false; b.playback.error = b.error_string(end->error);}
        }
        if (event->event_id == MPV_EVENT_COMMAND_REPLY) {
            auto pending = b.pending.find(event->reply_userdata);
            bool current = pending != b.pending.end() && pending->second == b.generation;
            b.pending.erase(event->reply_userdata);
            auto it = b.shots.find(event->reply_userdata);
            if (it == b.shots.end()) { if (current && event->error < 0) b.playback.error = b.error_string(event->error); continue; }
            auto shot = it->second; b.shots.erase(it); Pixels pixels; std::string error;
            try {
                if (event->error < 0) throw std::runtime_error(b.error_string(event->error));
                if (shot.generation != b.generation) throw std::runtime_error("Item changed before frame capture completed");
                auto& node = static_cast<mpv_event_command*>(event->data)->result;
                if (node.format != MPV_FORMAT_NODE_MAP) throw std::runtime_error("Invalid screenshot result");
                auto get = [&](const char* key)->mpv_node* {auto list = node.u.list; for (int n = 0;n < list->num;++n) if (!strcmp(list->keys[n],key)) return &list->values[n]; return nullptr;};
                auto w = get("w"), h = get("h"), stride = get("stride"), format = get("format"), data = get("data");
                if (!w || !h || !stride || !format || !data || w->format != MPV_FORMAT_INT64 || h->format != MPV_FORMAT_INT64 || stride->format != MPV_FORMAT_INT64 || format->format != MPV_FORMAT_STRING || data->format != MPV_FORMAT_BYTE_ARRAY) throw std::runtime_error("Invalid screenshot fields");
                if (w->u.int64 <= 0 || h->u.int64 <= 0 || w->u.int64 > 16384 || h->u.int64 > 16384 || w->u.int64*h->u.int64 > 64*1024*1024 || stride->u.int64 < w->u.int64*4 || static_cast<uint64_t>(stride->u.int64)*h->u.int64 > data->u.ba->size) throw std::runtime_error("Screenshot exceeds pixel budget");
                bool bgr = !strcmp(format->u.string,"bgr0") || !strcmp(format->u.string,"bgra");
                bool rgb = !strcmp(format->u.string,"rgb0") || !strcmp(format->u.string,"rgba");
                bool alpha = !strcmp(format->u.string,"bgra") || !strcmp(format->u.string,"rgba");
                if (!bgr && !rgb) throw std::runtime_error("Unsupported screenshot pixel format");
                pixels.width = static_cast<int>(w->u.int64); pixels.height = static_cast<int>(h->u.int64); pixels.bgra.resize(static_cast<size_t>(pixels.width)*pixels.height*4);
                auto source = static_cast<const unsigned char*>(data->u.ba->data);
                for (int y = 0;y < pixels.height;++y) for (int x = 0;x < pixels.width;++x) {
                    auto p = source+y*stride->u.int64+x*4; auto dest = pixels.bgra.data()+(static_cast<size_t>(y)*pixels.width+x)*4;
                    dest[0] = p[bgr ? 0 : 2]; dest[1] = p[1]; dest[2] = p[bgr ? 2 : 0]; dest[3] = alpha ? p[3] : 255;
                }
                // Software capture preserves alpha but leaves display rotation to the caller.
                int rotation = (shot.rotation%360+360)%360;
                if (rotation == 90 || rotation == 180 || rotation == 270) {
                    Pixels rotated; rotated.width = rotation == 180 ? pixels.width : pixels.height; rotated.height = rotation == 180 ? pixels.height : pixels.width; rotated.bgra.resize(pixels.bgra.size());
                    for (int y = 0;y < pixels.height;++y) for (int x = 0;x < pixels.width;++x) {
                        int dx = rotation == 90 ? pixels.height-1-y : rotation == 180 ? pixels.width-1-x : y;
                        int dy = rotation == 90 ? x : rotation == 180 ? pixels.height-1-y : pixels.width-1-x;
                        std::copy_n(pixels.bgra.data()+(static_cast<size_t>(y)*pixels.width+x)*4,4,rotated.bgra.data()+(static_cast<size_t>(dy)*rotated.width+dx)*4);
                    }
                    pixels = std::move(rotated);
                }
            } catch (const std::exception& e) {error = e.what();}
            captured(shot.request,shot.generation,std::move(pixels),error);
        }
    }
    auto& s = b.playback;
    if (s.ready) {
        s.position = b.number("time-pos"); s.duration = b.number("duration"); s.paused = b.flag("pause"); s.seeking = b.flag("seeking"); s.ended = b.flag("eof-reached");
        s.volume = b.number("volume"); s.muted = b.flag("mute"); s.width = static_cast<int>(b.number("video-params/dw")); s.height = static_cast<int>(b.number("video-params/dh")); s.decoder = b.string("hwdec-current");
    }
    s.bytesRead = b.bytes;
}
}
