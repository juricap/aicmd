#include "protocol.h"
#include <iostream>
using av::Json;
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--tools") {std::cout << av::tool_schemas().dump(2) << '\n'; return 0;}
        if (argc < 3 || std::wstring(argv[1]) != L"--pipe") throw std::runtime_error("Usage: aiview-tool --tools | --pipe aiview-PID [--mcp] (JSON lines on stdin)");
        bool mcp = argc == 4 && std::wstring(argv[3]) == L"--mcp";
        std::string line;
        for (;;) {
            line.clear(); bool oversized = false; char byte = 0;
            while (std::cin.get(byte) && byte != '\n') {if (line.size() < 1024*1024) line += byte; else oversized = true;}
            if (line.empty() && !std::cin) break;
            Json request, response;
            try {
                if (oversized) throw std::runtime_error("Request exceeds 1 MiB");
                request = Json::parse(line);
                if (!mcp) {std::cout << av::pipe_call(argv[2],request.dump()) << std::flush; continue;}
                if (!request.contains("id")) continue;
                response = {{"jsonrpc","2.0"},{"id",request["id"]}};
                auto method = request.at("method").get<std::string>();
                if (method == "initialize") response["result"] = {{"protocolVersion","2024-11-05"},{"capabilities",{{"tools",Json::object()}}},{"serverInfo",{{"name","aiview"},{"version","0.1.0"}}}};
                else if (method == "ping") response["result"] = Json::object();
                else if (method == "tools/list") response["result"] = {{"tools",av::tool_schemas()}};
                else if (method == "tools/call") {
                    auto result = Json::parse(av::pipe_call(argv[2],request.at("params").dump()));
                    Json content = Json::array();
                    if (result.value("ok",false) && result["result"].contains("mimeType") && result["result"].contains("data")) {
                        content.push_back({{"type","image"},{"mimeType",result["result"]["mimeType"]},{"data",result["result"]["data"]}}); result["result"].erase("data");
                    }
                    content.push_back({{"type","text"},{"text",result.dump()}});
                    response["result"] = {{"content",content},{"isError",!result.value("ok",false)}};
                } else response["error"] = {{"code",-32601},{"message","Unknown method"}};
            } catch (const std::exception& e) {
                response = mcp ? Json{{"jsonrpc","2.0"},{"id",request.value("id",Json(nullptr))},{"error",{{"code",-32602},{"message",e.what()}}}} : Json{{"ok",false},{"error",e.what()}};
            }
            std::cout << response.dump() << '\n' << std::flush;
        }
        return 0;
    } catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
