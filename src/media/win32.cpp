#include "backend.h"
#include "protocol.h"
#include "png.h"
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <sddl.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace av;
namespace {
std::wstring wide(const std::string& text) {return ac::fs::u8path(text).wstring();}
struct App {
    HWND window{},surface{}; Backend backend; Session session;
    bool initialized = false, fullscreen = false, closing = false;
    RECT normal{}; DWORD style = 0;
    std::future<std::pair<std::vector<Item>,size_t>> listing;
    ac::Cancel cancel = std::make_shared<std::atomic<bool>>(false);
    std::string error, title; double zoom = 0;
    uint64_t operation = 0; Json lastOperation = nullptr;
    bool captureForAgent = false;
    Json frame = nullptr;
    double capturePosition = 0;
    ac::fs::path returnPath;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::string incoming,outgoing; size_t written = 0; ULONGLONG connected = 0;
    bool pipeConnected = false;
    ~App() {cancel->store(true); backend.cancel(); if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);}
} *app;
Json state() {
    const auto& s = app->backend.state();
    Json j = {{"protocol",1},{"pid",GetCurrentProcessId()},{"pipe","aiview-"+std::to_string(GetCurrentProcessId())},
        {"generation",app->session.generation},{"count",app->session.items.size()},{"index",app->session.index},
        {"ready",s.ready && !app->listing.valid()},{"paused",s.paused},{"seeking",s.seeking},{"ended",s.ended},
        {"position",s.position},{"duration",s.duration},{"width",s.width},{"height",s.height},
        {"volume",s.volume},{"muted",s.muted},{"fullscreen",app->fullscreen},{"zoom",app->zoom},
        {"decoder",s.decoder},{"backend",s.version},{"remote_bytes_read",s.bytesRead},{"last_operation",app->lastOperation},
        {"error",app->error.empty() ? s.error : app->error}};
    if (!app->session.items.empty()) { auto& i = app->session.current(); j["path"] = i.location.u8string(); j["kind"] = i.type == Kind::Image ? "image" : i.type == Kind::Audio ? "audio" : "video"; }
    return j;
}
void load() { app->error.clear(); app->zoom = 0; app->backend.load(app->session.current(),app->session.generation); }
void fullscreen(bool on) {
    if (app->fullscreen == on) return;
    if (on) {
        app->style = static_cast<DWORD>(GetWindowLongPtrW(app->window,GWL_STYLE)); GetWindowRect(app->window,&app->normal);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfoW(MonitorFromWindow(app->window,MONITOR_DEFAULTTONEAREST),&mi);
        SetWindowLongPtrW(app->window,GWL_STYLE,app->style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(app->window,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(app->window,GWL_STYLE,app->style);
        const auto r = app->normal; SetWindowPos(app->window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_FRAMECHANGED);
    }
    app->fullscreen = on;
}
void clipboard(Pixels pixels) {
    const size_t count = pixels.bgra.size();
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPINFOHEADER)+count);
    if (!dib) throw std::runtime_error("Cannot allocate clipboard image");
    auto header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib));
    if (!header) {GlobalFree(dib); throw std::runtime_error("Cannot lock clipboard image");}
    *header = {}; header->biSize = sizeof(*header); header->biWidth = pixels.width; header->biHeight = -pixels.height;
    header->biPlanes = 1; header->biBitCount = 32; header->biCompression = BI_RGB; header->biSizeImage = static_cast<DWORD>(count);
    auto fallback = reinterpret_cast<unsigned char*>(header+1);
    for (size_t i = 0;i < count;i += 4) {
        for (size_t c = 0;c < 3;++c) fallback[i+c] = static_cast<unsigned char>((pixels.bgra[i+c]*pixels.bgra[i+3]+127)/255);
        fallback[i+3] = 255;
    }
    GlobalUnlock(dib);
    HGLOBAL dib5 = GlobalAlloc(GMEM_MOVEABLE,sizeof(BITMAPV5HEADER)+count);
    if (!dib5) {GlobalFree(dib); throw std::runtime_error("Cannot allocate alpha clipboard image");}
    auto v5 = static_cast<BITMAPV5HEADER*>(GlobalLock(dib5));
    if (!v5) {GlobalFree(dib); GlobalFree(dib5); throw std::runtime_error("Cannot lock alpha clipboard image");}
    *v5 = {}; v5->bV5Size = sizeof(*v5); v5->bV5Width = pixels.width; v5->bV5Height = -pixels.height; v5->bV5Planes = 1; v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS; v5->bV5SizeImage = static_cast<DWORD>(count); v5->bV5RedMask = 0x00ff0000; v5->bV5GreenMask = 0x0000ff00; v5->bV5BlueMask = 0x000000ff; v5->bV5AlphaMask = 0xff000000; v5->bV5CSType = LCS_sRGB;
    std::memcpy(v5+1,pixels.bgra.data(),count); GlobalUnlock(dib5);
    if (!OpenClipboard(app->window)) {GlobalFree(dib); GlobalFree(dib5); throw std::runtime_error("Clipboard is busy; press Ctrl+C again");}
    if (!EmptyClipboard() || !SetClipboardData(CF_DIB,dib)) {CloseClipboard(); GlobalFree(dib); GlobalFree(dib5); throw std::runtime_error("Cannot publish clipboard image");}
    if (!SetClipboardData(CF_DIBV5,dib5)) GlobalFree(dib5);
    CloseClipboard();
}
Json execute(const Command& c) {
    if (c.action == Action::Close) {
        if (c.generation && c.generation != app->session.generation) throw std::runtime_error("Stale media generation; get state again");
        PostMessageW(app->window,WM_CLOSE,0,0); return {{"accepted",true}};
    }
    if (app->listing.valid()) throw std::runtime_error("Folder is still loading");
    app->session.validate(c);
    if (app->session.navigate(c)) {load(); return {{"accepted",true},{"generation",app->session.generation}};}
    const auto& s = app->backend.state();
    switch (c.action) {
    case Action::Next: case Action::Previous: case Action::First: case Action::Last: case Action::Select: break;
    case Action::Play: case Action::Pause: case Action::TogglePause:
        if (app->session.current().type != Kind::Image) app->backend.command({"set","pause",c.action == Action::Pause || (c.action == Action::TogglePause && !s.paused) ? "yes" : "no"}); break;
    case Action::SeekRelative: case Action::SeekAbsolute:
        if (!s.ready) throw std::runtime_error("Media is not ready");
        app->backend.command({"seek",std::to_string(std::clamp(c.value+(c.action == Action::SeekRelative ? s.position : 0),0.0,std::max(0.0,s.duration))),"absolute+exact"}); break;
    case Action::StepFrame:
        if (!s.ready) throw std::runtime_error("Video is not ready");
        app->backend.command({"set","pause","yes"});
        app->backend.command({c.value > 0 ? "frame-step" : "frame-back-step"}); break;
    case Action::Volume: app->backend.command({"set","volume",std::to_string(c.value)}); break;
    case Action::Mute: app->backend.command({"set","mute",c.value ? "yes" : "no"}); break;
    case Action::Fullscreen: fullscreen(c.value != 0); break;
    case Action::ImageZoom: app->zoom = c.value; app->backend.command({"set","video-zoom",std::to_string(c.value)}); break;
    case Action::CopyFrame: case Action::CaptureFrame: {
        auto id = ++app->operation; app->backend.capture(id,app->session.generation);
        app->captureForAgent = c.action == Action::CaptureFrame; app->capturePosition = s.position;
        app->lastOperation = {{"id",id},{"generation",app->session.generation},{"status","pending"}};
        return {{"accepted",true},{"operation",id}};
    }
    case Action::Close: PostMessageW(app->window,WM_CLOSE,0,0); break;
    }
    return {{"accepted",true},{"generation",app->session.generation}};
}
void scan(const ac::Location& file) {
    if (app->listing.valid()) throw std::runtime_error("Folder is still loading");
    app->backend.cancel(); app->backend.command({"stop"}); app->error.clear(); auto cancel = app->cancel;
    app->listing = std::async(std::launch::async,[file,cancel] {
        auto list = folder(file,cancel); size_t index = 0;
        for (size_t i = 0;i < list.size();++i) if (list[i].location == file) index = i;
        return std::make_pair(std::move(list),index);
    });
}
Json request(const Json& j) {
    try {
        if (!j.is_object()) throw std::runtime_error("Request must be an object");
        auto name = j.at("name").get<std::string>(); Json result;
        if (name == "media_get_state") result = state();
        else if (name == "media_get_capabilities") result = {{"protocol",1},{"tools",tool_schemas()},{"clipboard","CF_DIBV5 with alpha plus black-composited CF_DIB fallback; SDR decoded pixels; no window chrome"},{"capture","PNG, longest side at most 768; explicit media_capture_frame then media_get_frame"},{"remote","Seekable SFTP with 64 MiB block cache"}};
        else if (name == "media_get_frame") {
            if (app->frame.is_null() || j.at("arguments").at("operation") != app->frame.at("operation")) throw std::runtime_error("No completed frame for this operation");
            if (app->frame.at("generation") != app->session.generation) throw std::runtime_error("Captured item is stale; capture the current frame again");
            result = app->frame;
        }
        else if (name == "media_get_items") {
            const auto& value = j.at("arguments").at("offset"); if (!value.is_number_unsigned()) throw std::runtime_error("offset must be a nonnegative integer");
            auto offset = value.get<size_t>(); if (offset > app->session.items.size()) throw std::runtime_error("offset exceeds folder size");
            Json items = Json::array(); auto end = std::min(offset+128,app->session.items.size());
            for (auto i = offset;i < end;++i) items.push_back({{"index",i},{"path",app->session.items[i].location.u8string()}});
            result = {{"generation",app->session.generation},{"items",items},{"next_offset",end},{"count",app->session.items.size()}};
        }
        else if (name == "media_open") {
            const auto args = j.at("arguments"); auto path = args.at("path").get<std::string>();
            if (path.find('\0') != std::string::npos) throw std::runtime_error("NUL in path");
            auto location = ac::absolute_path(ac::Location(ac::fs::u8path(path)),ac::Location(ac::fs::current_path()));
            scan(location); result = {{"accepted",true},{"opening",path}};
        } else result = execute(parse_command(j));
        return {{"ok",true},{"result",result}};
    } catch (const std::exception& e) {return {{"ok",false},{"error",e.what()}};}
}
void make_pipe() {
    HANDLE token{}; if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) throw std::runtime_error("Cannot determine IPC user");
    DWORD size = 0; GetTokenInformation(token,TokenUser,nullptr,0,&size); std::vector<char> bytes(size);
    if (!GetTokenInformation(token,TokenUser,bytes.data(),size,&size)) {CloseHandle(token); throw std::runtime_error("Cannot read IPC user");} CloseHandle(token);
    LPWSTR sid{}; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid)) throw std::runtime_error("Cannot identify IPC user");
    auto sddl = std::wstring(L"D:P(A;;GA;;;")+sid+L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)) throw std::runtime_error("Cannot secure IPC pipe");
    SECURITY_ATTRIBUTES sa{sizeof(sa),descriptor,FALSE}; auto name = L"\\\\.\\pipe\\aiview-"+std::to_wstring(GetCurrentProcessId());
    app->pipe = CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_NOWAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&sa); LocalFree(descriptor);
    if (app->pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create viewer IPC pipe");
}
void poll_pipe() {
    if (!app->pipeConnected) {
        if (!ConnectNamedPipe(app->pipe,nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) return;
        app->pipeConnected = true; app->connected = GetTickCount64();
    }
    bool disconnect = GetTickCount64()-app->connected > 10000;
    char data[8192]; DWORD n = 0;
    if (app->outgoing.empty()) {
        for (int i = 0;i < 16;++i) {
            if (!ReadFile(app->pipe,data,sizeof(data),&n,nullptr)) {if (GetLastError() != ERROR_NO_DATA) disconnect = true; break;}
            if (!n) break; app->incoming.append(data,n);
            if (app->incoming.size() > 1024*1024) {disconnect = true; break;}
            auto end = app->incoming.find('\n'); if (end != std::string::npos) {
                try {app->outgoing = request(Json::parse(app->incoming.substr(0,end))).dump()+"\n";}
                catch (const std::exception& e) {app->outgoing = Json{{"ok",false},{"error",e.what()}}.dump()+"\n";}
                break;
            }
        }
    }
    if (!app->outgoing.empty() && app->written < app->outgoing.size()) {
        // Nonblocking pipes need bounded chunks; a large PNG may exceed the pipe buffer.
        for (int i = 0;i < 8 && app->written < app->outgoing.size();++i) {
            auto count = static_cast<DWORD>(std::min<size_t>(16384,app->outgoing.size()-app->written));
            if (!WriteFile(app->pipe,app->outgoing.data()+app->written,count,&n,nullptr)) {if (GetLastError() != ERROR_NO_DATA) disconnect = true; break;}
            if (!n) break;
            app->written += n; app->connected = GetTickCount64();
        }
        // Let the client consume its response and close. Never FlushFileBuffers on the UI thread.
    } else if (!app->outgoing.empty()) {DWORD available = 0; if (!PeekNamedPipe(app->pipe,nullptr,0,nullptr,&available,nullptr)) disconnect = true;}
    if (disconnect) {DisconnectNamedPipe(app->pipe); app->pipeConnected = false; app->incoming.clear(); app->outgoing.clear(); app->written = 0;}
}
void tick() {
    if (app->listing.valid() && app->listing.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        try {auto result = app->listing.get(); app->session.replace(std::move(result.first),result.second); load();}
        catch (const std::exception& e) {app->error = e.what();}
    }
    app->backend.poll([](uint64_t id,uint64_t generation,Pixels pixels,std::string error) {
        if (generation != app->session.generation) error = "Item changed before copy completed";
        if (error.empty()) try {
            if (app->captureForAgent) {
                int width = 0,height = 0; auto data = preview_png(pixels,width,height);
                app->frame = {{"operation",id},{"generation",generation},{"mimeType","image/png"},{"data",std::move(data)},{"width",width},{"height",height},{"position_requested",app->capturePosition}};
            } else clipboard(std::move(pixels));
        } catch (const std::exception& e) {error = e.what();}
        app->lastOperation = {{"id",id},{"generation",generation},{"status",error.empty() ? "completed" : "failed"},{"error",error}};
        if (!error.empty()) app->error = error;
        else {app->error.clear(); app->backend.command({"show-text",app->captureForAgent ? "Frame captured for tool caller" : "Copied image to clipboard","1500"});}
    });
    poll_pipe();
    std::ostringstream title; title << "aiview";
    if (!app->session.items.empty()) title << " - " << ac::safe_text(app->session.current().location.filename().u8string()) << " [" << app->session.index+1 << '/' << app->session.items.size() << ']';
    const auto& s = app->backend.state();
    if (s.ready && app->session.current().type != Kind::Image) title << "  " << std::fixed << std::setprecision(1) << s.position << " / " << s.duration << "s" << (s.paused ? "  Paused" : "");
    if (app->listing.valid()) title << "  Loading folder...";
    if (!app->error.empty() || !s.error.empty()) title << "  " << (app->error.empty() ? s.error : app->error);
    if (title.str() != app->title) {app->title = title.str(); SetWindowTextW(app->window,wide(app->title).c_str());}
}
bool key(const MSG& msg) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    bool ctrl = (GetKeyState(VK_CONTROL)&0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU)&0x8000) != 0;
    if (alt) return false;
    if (msg.wParam == VK_ESCAPE) {PostMessageW(app->window,WM_CLOSE,0,0); return true;}
    Command c{Action::Next};
    if (ctrl && (msg.wParam == 'C' || msg.wParam == VK_INSERT)) c.action = Action::CopyFrame;
    else if (ctrl && (msg.wParam == VK_UP || msg.wParam == VK_DOWN)) {c.action = Action::Volume; c.value = std::clamp(app->backend.state().volume+(msg.wParam == VK_UP ? 5 : -5),0.0,100.0);}
    else if (ctrl) return false;
    else switch (msg.wParam) {
    case VK_NEXT: c.action = Action::Next; break;
    case VK_PRIOR: c.action = Action::Previous; break;
    case VK_HOME: c.action = Action::First; break;
    case VK_END: c.action = Action::Last; break;
    case VK_LEFT: case VK_RIGHT: c.action = Action::SeekRelative; c.value = msg.wParam == VK_RIGHT ? 5 : -5; break;
    case VK_OEM_COMMA: case VK_OEM_PERIOD: c.action = Action::StepFrame; c.value = msg.wParam == VK_OEM_PERIOD ? 1 : -1; break;
    case VK_SPACE: c.action = Action::TogglePause; break;
    case 'F': c.action = Action::Fullscreen; c.value = !app->fullscreen; break;
    case 'M': c.action = Action::Mute; c.value = !app->backend.state().muted; break;
    case VK_ADD: case VK_OEM_PLUS: c.action = Action::ImageZoom; c.value = std::min(10.0,app->zoom+0.25); break;
    case VK_SUBTRACT: case VK_OEM_MINUS: c.action = Action::ImageZoom; c.value = std::max(-10.0,app->zoom-0.25); break;
    case '0': c.action = Action::ImageZoom; break;
    case '1': {
        RECT r{}; GetClientRect(app->surface,&r); const auto& s = app->backend.state();
        if (!s.width || !s.height) return true;
        c.action = Action::ImageZoom; c.value = std::log2(std::max(static_cast<double>(s.width)/std::max(1L,r.right),static_cast<double>(s.height)/std::max(1L,r.bottom))); break;
    }
    default: return false;
    }
    // Avoid accidental repeated copy/fullscreen/close actions while a key is held.
    if ((msg.lParam & (1LL<<30)) && (c.action == Action::CopyFrame || c.action == Action::Fullscreen)) return true;
    try {execute(c); app->error.clear();} catch (const std::exception& e) {app->error = e.what();}
    return true;
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if (message == WM_SIZE && app && app->surface) {MoveWindow(app->surface,0,0,LOWORD(l),HIWORD(l),TRUE); return 0;}
    if (message == WM_SETFOCUS && app && app->surface) {SetFocus(app->surface); return 0;}
    if (message == WM_TIMER && app && app->initialized) {try {tick();} catch (const std::exception& e) {app->error = e.what();} return 0;}
    if (message == WM_CLOSE) {
        if (!app->returnPath.empty() && !app->session.items.empty()) {std::ofstream out(app->returnPath,std::ios::binary); out << Json{{"path",app->session.current().location.u8string()}}.dump();}
        app->cancel->store(true); app->backend.cancel(); app->closing = true; ShowWindow(window,SW_HIDE); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    App application; app = &application;
    try {
        if (FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) throw std::runtime_error("Cannot initialize native image encoder");
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(),&argc);
        ac::fs::path sessionPath; ac::Location file;
        for (int i = 1;i < argc;++i) {
            std::wstring arg(argv[i]);
            if (arg == L"--session" && i+1 < argc) sessionPath = argv[++i];
            else if (arg == L"--return" && i+1 < argc) app->returnPath = argv[++i];
            else if (arg == L"--" && i+1 < argc) {file = ac::Location(std::wstring(argv[++i])); break;}
            else file = ac::Location(arg);
        }
        LocalFree(argv);
        WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"aiview-window"; wc.hCursor = LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); RegisterClassW(&wc);
        app->window = CreateWindowW(wc.lpszClassName,L"aiview",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1100,740,nullptr,nullptr,instance,nullptr);
        if (!app->window) throw std::runtime_error("Cannot create viewer window");
        app->surface = CreateWindowW(L"STATIC",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|SS_BLACKRECT,0,0,1084,701,app->window,nullptr,instance,nullptr);
        app->backend.initialize(reinterpret_cast<uintptr_t>(app->surface)); make_pipe(); app->initialized = true;
        if (!sessionPath.empty()) {
            app->listing = std::async(std::launch::async,[sessionPath] {
                if (ac::fs::file_size(sessionPath) > 32*1024*1024) throw std::runtime_error("Folder snapshot exceeds 32 MiB");
                std::ifstream input(sessionPath,std::ios::binary); Json j; input >> j;
                if (j.at("version") != 1) throw std::runtime_error("Unsupported folder snapshot");
                std::vector<Item> list;
                for (const auto& name : j.at("items")) {auto value = name.get<std::string>(); if (value.find('\0') != std::string::npos) throw std::runtime_error("NUL in media path"); ac::Location l(ac::fs::u8path(value)); list.push_back({l,kind(l.filename())}); if (list.size() > 100000) throw std::runtime_error("Too many folder items");}
                return std::make_pair(std::move(list),j.at("index").get<size_t>());
            });
        } else if (!file.empty()) scan(ac::absolute_path(file,ac::Location(ac::fs::current_path())));
        ShowWindow(app->window,show); SetTimer(app->window,1,30,nullptr);
        MSG msg{}; while (GetMessageW(&msg,nullptr,0,0) > 0) {if (!key(msg)) {TranslateMessage(&msg); DispatchMessageW(&msg);}}
        KillTimer(app->window,1);
        // Keep the native surface alive until libmpv has finished shutting down.
        return 0;
    } catch (const std::exception& e) {MessageBoxW(nullptr,wide(e.what()).c_str(),L"aiview",MB_OK|MB_ICONERROR); return 1;}
}
