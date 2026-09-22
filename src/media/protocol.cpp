#include "protocol.h"
#include <windows.h>
#include <stdexcept>
namespace av {
struct Description { const char* name; const char* description; Action action; const char* parameter; };
static const Description commands[] = {
    {"media_next","Next item in the frozen folder order",Action::Next,nullptr},
    {"media_previous","Previous folder item",Action::Previous,nullptr},
    {"media_first","First folder item",Action::First,nullptr},
    {"media_last","Last folder item",Action::Last,nullptr},
    {"media_select","Select a zero-based folder index",Action::Select,"index"},
    {"media_play","Resume playback",Action::Play,nullptr},
    {"media_pause","Pause playback",Action::Pause,nullptr},
    {"media_seek_relative","Seek by signed seconds, preserving pause",Action::SeekRelative,"seconds"},
    {"media_seek_absolute","Seek to seconds from start, preserving pause",Action::SeekAbsolute,"seconds"},
    {"media_step_frame","Step one presentation frame and pause; direction must be -1 or 1",Action::StepFrame,"direction"},
    {"media_set_volume","Set volume, 0 to 100",Action::Volume,"value"},
    {"media_set_mute","Set mute (0 or 1)",Action::Mute,"value"},
    {"media_set_fullscreen","Set fullscreen (0 or 1)",Action::Fullscreen,"value"},
    {"media_set_image_view","Image zoom as a power of two; 0 fits, range -10..10",Action::ImageZoom,"value"},
    {"media_copy_frame","Copy the decoded image/video frame to the OS clipboard. Returns an operation ID; poll media_get_state for completion. Changes clipboard, never uploads media.",Action::CopyFrame,nullptr},
    {"media_capture_frame","Capture the current image/frame for agent inspection, without changing clipboard. Returns an operation ID; poll state then media_get_frame for PNG (maximum dimension 768).",Action::CaptureFrame,nullptr},
    {"media_close","Close this viewer",Action::Close,nullptr}
};
Json tool_schemas() {
    Json tools = Json::array();
    for (const auto& d : commands) {
        Json properties = {{"generation",{{"type","integer"},{"minimum",1},{"description","Expected item generation from media_get_state; stale commands fail"}}}};
        Json required = Json::array();
        if (d.parameter) { properties[d.parameter] = {{"type","number"}}; required.push_back(d.parameter); }
        tools.push_back({{"name",d.name},{"description",d.description},{"inputSchema",{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}}}});
    }
    for (auto name : {"media_get_state","media_get_capabilities"}) tools.push_back({{"name",name},{"description",name == std::string("media_get_state") ? "Read item/generation, playback and last clipboard operation state" : "List implemented capabilities and tools"},{"inputSchema",{{"type","object"},{"properties",Json::object()},{"additionalProperties",false}}}});
    tools.push_back({{"name","media_open"},{"description","Open a local file or ssh://alias/path and asynchronously snapshot its folder. Poll state until ready."},{"inputSchema",{{"type","object"},{"properties",{{"path",{{"type","string"}}}}},{"required",Json::array({"path"})},{"additionalProperties",false}}}});
    tools.push_back({{"name","media_get_frame"},{"description","Retrieve a completed media_capture_frame operation as a PNG image. This explicitly sends captured pixels to the caller."},{"inputSchema",{{"type","object"},{"properties",{{"operation",{{"type","integer"},{"minimum",1}}}}},{"required",Json::array({"operation"})},{"additionalProperties",false}}}});
    tools.push_back({{"name","media_get_items"},{"description","Read a page of the frozen folder snapshot, up to 128 items, with current generation"},{"inputSchema",{{"type","object"},{"properties",{{"offset",{{"type","integer"},{"minimum",0}}}}},{"required",Json::array({"offset"})},{"additionalProperties",false}}}});
    return tools;
}
Command parse_command(const Json& request) {
    auto name = request.at("name").get<std::string>(); const auto args = request.value("arguments",Json::object());
    if (!args.is_object()) throw std::runtime_error("arguments must be an object");
    for (const auto& d : commands) if (name == d.name) {
        for (auto it = args.begin();it != args.end();++it) if (it.key() != "generation" && (!d.parameter || it.key() != d.parameter)) throw std::runtime_error("Unknown argument: "+it.key());
        Command c{d.action};
        if (args.contains("generation")) { if (!args["generation"].is_number_unsigned() || args["generation"].get<uint64_t>() == 0) throw std::runtime_error("generation must be a positive integer"); c.generation = args["generation"].get<uint64_t>(); }
        if (d.parameter) c.value = args.at(d.parameter).get<double>();
        if ((c.action == Action::Mute || c.action == Action::Fullscreen) && c.value != 0 && c.value != 1) throw std::runtime_error("value must be 0 or 1");
        return c;
    }
    throw std::runtime_error("Unknown media tool");
}
std::string pipe_call(const std::wstring& name,const std::string& request) {
    if (name.rfind(L"aiview-",0) != 0 || name.find_first_not_of(L"aiview-0123456789") != std::wstring::npos) throw std::runtime_error("Invalid viewer pipe name");
    auto path = L"\\\\.\\pipe\\"+name;
    if (request.size() > 1024*1024) throw std::runtime_error("Request exceeds 1 MiB");
    WaitNamedPipeW(path.c_str(),5000);
    HANDLE pipe = CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Viewer pipe unavailable");
    HANDLE event = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto transfer = [&](bool write,char* data,DWORD size)->DWORD {
        OVERLAPPED ov{}; ov.hEvent = event; ResetEvent(event); DWORD done = 0;
        BOOL ok = write ? WriteFile(pipe,data,size,&done,&ov) : ReadFile(pipe,data,size,&done,&ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(event,15000) != WAIT_OBJECT_0) { CancelIoEx(pipe,&ov); GetOverlappedResult(pipe,&ov,&done,TRUE); throw std::runtime_error("Viewer request timed out"); }
            ok = GetOverlappedResult(pipe,&ov,&done,FALSE);
        }
        if (!ok) throw std::runtime_error("Viewer pipe disconnected"); return done;
    };
    try {
        auto input = request+"\n"; size_t at = 0;
        while (at < input.size()) { auto n = transfer(true,input.data()+at,static_cast<DWORD>(input.size()-at)); if (!n) throw std::runtime_error("Pipe write failed"); at += n; }
        std::string response; char data[4096];
        while (response.find('\n') == std::string::npos) {auto n = transfer(false,data,sizeof(data)); if (!n || response.size()+n > 4*1024*1024) throw std::runtime_error("Invalid viewer response"); response.append(data,n);}
        CloseHandle(event); CloseHandle(pipe); return response;
    } catch (...) {CloseHandle(event); CloseHandle(pipe); throw;}
}
}
