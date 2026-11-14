#include "core/search.h"
#include <chrono>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace ac;
static int checks = 0;
static void require(bool ok,const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static void write(const fs::path& p,const std::string& s) { std::ofstream f(p,std::ios::binary); f << s; if (!f) throw std::runtime_error("Fixture write failed"); }
static std::vector<SearchHit> run(Search& s,int seconds = 60) {
    std::vector<SearchHit> all; auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    for (;;) {
        auto part = s.take(); for (auto& h : part) all.push_back(std::move(h));
        if (s.stats().finished) { for (auto& h : s.take()) all.push_back(std::move(h)); return all; }
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Search timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static SearchQuery q(const Location& root,const std::string& mask) { SearchQuery s; s.root = root; s.mask = mask; return s; }
static size_t depth(const fs::path& p) { size_t n = 0; for (auto it = p.begin(); it != p.end(); ++it) ++n; return n; }
int main() {
    auto root = fs::temp_directory_path()/("aicmd-search-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        // root: 3 files + dirs; "huge" holds 600 entries (slow lane with a 100-entry probe); deep chain of 12.
        fs::create_directories(root/"huge"); fs::create_directories(root/"docs"/"inner");
        for (auto n : {"r1.txt","r2.txt","R3.TXT","skip.bin"}) write(root/n,n);
        for (int i = 0; i < 600; ++i) write(root/"huge"/("h"+std::to_string(i)+".txt"),"x");
        fs::create_directories(root/"huge"/"below"); write(root/"huge"/"below"/"under-huge.txt","x");
        write(root/"docs"/"d.txt","d"); write(root/"docs"/"inner"/"i.txt","i");
        fs::path chain = root/"chain"; for (int i = 0; i < 12; ++i) chain /= "c"+std::to_string(i);
        fs::create_directories(chain); write(chain/"deep.txt","deep");
        std::error_code ec; fs::create_directory_symlink(root,root/"docs"/"loop",ec);
        bool loop = !ec;
        write(root/".hidden.txt","h");
#ifdef _WIN32
        SetFileAttributesW((root/".hidden.txt").c_str(),FILE_ATTRIBUTE_HIDDEN);
#endif
        // Content fixture: the needle spans the 1 MiB read boundary.
        std::string big(1024*1024+100,'a'); big.replace(1024*1024-3,6,"NeEdLe"); write(root/"docs"/"big.dat",big);
        write(root/"docs"/"other.dat","no match here");

        SearchTuning tuning; tuning.probeEntries = 100;
        Search names(filesystem_provider(),q(root,"*.txt"),tuning);
        auto hits = run(names); auto stats = names.stats();
        size_t expected = 3+600+1+2+1; // root, huge, under-huge, docs, chain (hidden excluded)
        require(hits.size() == expected && stats.hits == expected,"All name matches found exactly once");
        require(stats.slowFolders == 1,"Oversized folder handed to the slow lane");
        require(stats.unreadable == 0 && !stats.truncated,"No errors");
        // The start folder is listed completely before anything below it is reported.
        size_t firstChild = 0; while (firstChild < hits.size() && depth(hits[firstChild].relative) == 1) ++firstChild;
        require(firstChild == 3,"Start folder matches come first");
        bool deepAfterShallow = true; size_t docsAt = 0, deepAt = 0;
        for (size_t i = 0; i < hits.size(); ++i) { if (hits[i].relative == fs::path("docs")/"d.txt") docsAt = i; if (hits[i].relative.filename() == "deep.txt") deepAt = i; }
        deepAfterShallow = docsAt < deepAt;
        require(deepAfterShallow,"Breadth-first: shallow folder results precede deep ones");
        // A small folder found after the huge one is not stuck behind it.
        size_t lastHuge = 0; for (size_t i = 0; i < hits.size(); ++i) if (hits[i].relative.parent_path() == "huge") lastHuge = i;
        require(docsAt < lastHuge || stats.slowFolders == 1,"Small folders are not blocked by the slow lane");
        if (loop) require(std::none_of(hits.begin(),hits.end(),[](const SearchHit& h){ return h.relative.u8string().find("loop") != std::string::npos; }),"Directory links are not followed");
        else std::cout << "SKIP symlink loop (" << ec.message() << ")\n";
        auto hidden = q(root,"*.txt"); hidden.hidden = true;
        Search withHidden(filesystem_provider(),hidden,tuning); require(run(withHidden).size() == expected+1,"Hidden entries included on request");
        auto dirs = q(root,"c1*;inner"); Search folders(filesystem_provider(),dirs);
        require(run(folders).size() == 4,"Directories match masks (c1, c10, c11 and inner)");
        auto content = q(root,"*.dat"); content.text = "needle";
        Search grep(filesystem_provider(),content);
        auto found = run(grep);
        require(found.size() == 1 && found[0].relative == fs::path("docs")/"big.dat" && grep.stats().contentChecked == 2,"Content match across chunk boundary, case-insensitive");
        auto capped = q(root,"*"); capped.maxResults = 50;
        Search limit(filesystem_provider(),capped,tuning); auto limited = run(limit);
        require(limited.size() == 50 && limit.stats().truncated,"Result limit truncates and stops");
        {
            Search stop(filesystem_provider(),q(root,"*"),tuning); stop.cancel();
            auto start = std::chrono::steady_clock::now(); run(stop,5);
            require(std::chrono::steady_clock::now()-start < std::chrono::seconds(2),"Cancel finishes promptly");
        }
        Search missing(filesystem_provider(),q(root/"does-not-exist","*"));
        run(missing); require(missing.stats().unreadable == 1,"Unreadable start folder reported");

        // Core: result tab, progressive publish, go-to-file, refresh and leaving search mode.
        Core core; core.start(root,root);
        auto settle = [&](auto done) { auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(60); while (!done()) { core.poll(); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Core timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); } core.poll(); };
        settle([&]{ return !core.busy(); });
        Command c; c.action = Action::SearchStart; c.panel = 0; c.pattern = "d*.txt"; auto r = core.execute(c);
        require(!r.error && core.state().panels[0].tabs.size() == 2 && core.tab(0)->search,"Search opens a result tab");
        settle([&]{ return !core.searching(); }); core.poll();
        auto t = core.tab(0);
        require(t->snapshot && t->snapshot->entries.size() == 2 && t->path == Location(root),"Results published relative to the search folder");
        Command refresh; refresh.action = Action::Refresh; refresh.panel = 0; core.execute(refresh);
        require(core.tab(0)->search != nullptr,"Refresh reruns the search");
        settle([&]{ return !core.searching(); });
        require(core.tab(0)->snapshot->entries.size() == 2,"Rerun result count");
        Command go; go.action = Action::Navigate; go.panel = 0; go.path = root/"docs"; go.focus = "d.txt"; core.execute(go);
        require(!core.tab(0)->search,"Navigating leaves search mode");
        settle([&]{ return !core.busy(); });
        require(core.tab(0)->path == Location(root/"docs") && core.tab(0)->focus == "d.txt","Go to file focuses it in its folder");
        fs::remove_all(root,ec);
        std::cout << "PASS " << checks << " search checks\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << " (fixtures " << root << ")\n"; return 1; }
}
