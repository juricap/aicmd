#include "core/core.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
using namespace ac;
int run(const std::vector<fs::path>& args) {
    if (args.size() > 1 && args[1] == "commands") {
        for (auto& c : commands()) std::cout << c.name << '\t' << c.effect << '\t' << c.description << '\n';
        return 0;
    }
    if (args.size() < 3 || (args[1] != "list" && args[1] != "bench")) {
        std::cout << "aicmd-cli commands | list <directory> | bench <directory> [runs]\n"; return args.size() == 1 ? 0 : 2;
    }
    int runs = args[1] == "bench" ? 20 : 1;
    if (args.size() > 3) { try { runs = std::clamp(std::stoi(args[3].string()),1,100); } catch (...) { return 2; } }
    auto path = absolute_path(args[2],fs::current_path());
    Core core; core.start(path,path);
    for (int run = 0; run < runs; ++run) {
        auto start = std::chrono::steady_clock::now();
        if (run) { Command c; c.action = Action::Refresh; core.execute(c); }
        while (core.busy()) {
            core.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (std::chrono::steady_clock::now()-start > std::chrono::seconds(30)) { std::cerr << "Scan timeout\n"; return 3; }
        }
        auto t = core.tab(0);
        if (t->error) { std::cerr << t->error.code << ": " << t->error.message << '\n'; return 1; }
        if (args[1] == "bench") {
            auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::cout << "run=" << run+1 << " entries=" << t->snapshot->entries.size() << " ms=" << ms << '\n';
        } else for (const auto& e : t->snapshot->entries)
            std::cout << (e.directory ? "DIR " : "FILE") << '\t' << (e.size ? std::to_string(*e.size) : "-") << '\t' << display_path(e.name) << '\n';
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try { std::vector<fs::path> args; for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]); return run(args); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
