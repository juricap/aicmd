#include "core/core.h"
#include "fs/ssh.h"
#include "core/search.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
using namespace ac;
int run(const std::vector<fs::path>& args) {
    if (args.size() == 2 && args[1] == "ssh-hosts") { for (const auto& host : ssh_hosts()) std::cout << host << '\n'; return 0; }
    if (args.size() == 4 && args[1] == "copy") {
        auto source = absolute_path(Location(args[2]),Location(fs::current_path()));
        auto destination = absolute_path(Location(args[3]),Location(fs::current_path()));
        uint64_t bytes = 0; auto start = std::chrono::steady_clock::now();
        copy_location(source,destination,std::make_shared<std::atomic<bool>>(false),[&](uint64_t n){bytes = n;});
        std::cout << "Copied " << bytes << " bytes in " << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << " s\n"; return 0;
    }
    if ((args.size() == 4 || args.size() == 5) && args[1] == "search") {
        SearchQuery q; q.root = absolute_path(Location(args[2]),Location(fs::current_path())); q.mask = args[3].u8string();
        if (args.size() == 5) q.text = args[4].u8string();
        auto start = std::chrono::steady_clock::now(); double first = -1;
        Search search(filesystem_provider(),q);
        for (;;) {
            auto hits = search.take();
            if (!hits.empty() && first < 0) first = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            for (const auto& h : hits) std::cout << display_path(h.relative) << '\n';
            if (search.stats().finished && hits.empty()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto s = search.stats();
        std::cerr << "hits=" << s.hits << " folders=" << s.folders << " entries=" << s.entries << " slow_lane=" << s.slowFolders << " unreadable=" << s.unreadable
                  << " first_hit_ms=" << first << " total_ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count() << (s.truncated ? " truncated" : "") << '\n';
        return 0;
    }
    if (args.size() > 1 && args[1] == "commands") {
        for (auto& c : commands()) std::cout << c.name << '\t' << c.effect << '\t' << c.description << '\n';
        return 0;
    }
    if (args.size() < 3 || (args[1] != "list" && args[1] != "bench")) {
        std::cout << "aicmd-cli commands | ssh-hosts | list <directory or ssh://alias/path> | bench <directory> [runs] | copy <source> <destination> | search <directory> <mask> [text]\n"; return args.size() == 1 ? 0 : 2;
    }
    int runs = args[1] == "bench" ? 20 : 1;
    if (args.size() > 3) { try { runs = std::clamp(std::stoi(args[3].string()),1,100); } catch (...) { return 2; } }
    auto path = absolute_path(Location(args[2]),Location(fs::current_path()));
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
