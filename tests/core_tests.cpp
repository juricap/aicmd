#include "core/core.h"
#include "host/state.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace ac;
static int checks = 0;
static void require(bool yes,const char* message) { ++checks; if (!yes) throw std::runtime_error(message); }
static void settle(Core& c) {
    auto start = std::chrono::steady_clock::now();
    do { c.poll(); if (!c.busy()) return; std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    while (std::chrono::steady_clock::now()-start < std::chrono::seconds(10));
    throw std::runtime_error("Worker timeout");
}
class Fake : public Provider {
public:
    std::atomic<int> entered{0};
    std::vector<Location> roots() override { return {fs::current_path().root_path()}; }
    Listing list(const Location& p,bool dirs,const Cancel&,size_t) override {
        if (p.filename() == "slow") { ++entered; std::this_thread::sleep_for(std::chrono::milliseconds(150)); }
        if (p.filename() == "denied") return {{},{"access_denied","Test denial"}};
        Listing r; Entry e; e.id = 1; e.name = dirs ? fs::path("child") : p.filename(); e.directory = dirs; r.entries.push_back(e); r.bytes = 256; return r;
    }
};
static Command cmd(Action a,int p = 0) { Command c; c.action = a; c.panel = p; return c; }
static void races() {
    auto fake = std::make_shared<Fake>(); Core c(fake); auto base = fs::current_path(); c.start(base/"start",base/"right"); settle(c);
    auto nav = cmd(Action::Navigate); nav.path = base/"slow"; auto slow = c.execute(nav);
    for (int n = 0; !fake->entered && n < 1000; ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(fake->entered > 0,"Slow request entered provider");
    nav.path = base/"fast"; c.execute(nav); settle(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(180)); c.poll();
    require(c.tab(0)->path == base/"fast","Late result cannot replace newer path");
    require(c.job(slow.job)->state == JobState::Cancelled,"Superseded job cancelled");
    auto oldHistory = c.tab(0)->history;
    nav.path = base/"denied"; c.execute(nav); settle(c);
    require(c.tab(0)->path == base/"fast","Failed navigation preserves path");
    require(c.tab(0)->history == oldHistory,"Failed navigation preserves history");
    require(c.tab(0)->snapshot->path == base/"fast","Failed navigation preserves snapshot");
    require(c.tab(0)->error.code == "access_denied","Structured provider error");
    auto create = cmd(Action::TabCreate); auto newTab = c.execute(create); settle(c);
    require(c.state().panels[0].tabs.size() == 2,"Create tab");
    require(c.tab(1)->path == base/"right","Other panel is independent");
    nav.tab = newTab.tab; nav.path = base/"slow"; c.execute(nav); c.execute(cmd(Action::TabClose)); settle(c);
    std::this_thread::sleep_for(std::chrono::milliseconds(180)); c.poll();
    require(!c.tab(0,newTab.tab),"Closed tab ignores late work");
    require(c.execute(cmd(Action::TabClose)).error.code == "last_tab","Last tab protected");
    auto conflict = cmd(Action::PanelActivate); conflict.expectedRevision = c.state().revision-1;
    require(c.execute(conflict).error.code == "revision_conflict","Stale revision rejected");
    nav.tab = 0; nav.path = base/"next"; c.execute(nav); settle(c);
    c.execute(cmd(Action::Back)); settle(c); require(c.tab(0)->path == base/"fast","Back works");
    c.execute(cmd(Action::Forward)); settle(c); require(c.tab(0)->path == base/"next","Forward works");
    auto selection = cmd(Action::SelectionSet); selection.snapshot = c.tab(0)->snapshot->id-1;
    require(c.execute(selection).error.code == "stale_snapshot","Old selection snapshot rejected");
    auto node = c.tab(0)->roots.front(); auto tree = cmd(Action::TreeExpand); tree.node = node;
    c.execute(tree); settle(c); auto count = c.tab(0)->tree.at(node).children.size();
    c.execute(tree); settle(c); require(c.tab(0)->tree.at(node).children.size() == count,"Tree re-expansion doesn't duplicate children");
    require(c.execute(cmd(Action::FileView)).error.code == "stale_snapshot","Viewer validates explicit snapshot");
}
static void real_fs(const fs::path& root) {
    fs::create_directories(root/"folder"/"nested"); fs::create_directories(root/"empty");
    auto unicode = fs::u8path(u8"日本語 space 😀.txt");
    { std::ofstream f(root/unicode); f << "sample"; }
    { std::ofstream f(root/"tiny.txt"); f << "x"; }
    { std::ofstream f(root/".hidden"); f << "hidden"; }
#ifdef _WIN32
    SetFileAttributesW((root/".hidden").c_str(),FILE_ATTRIBUTE_HIDDEN);
#endif
    Core c; c.start(root,root/"empty"); settle(c);
    require(!c.tab(0)->error,"Local filesystem load");
    require(c.tab(0)->snapshot->entries.size() == 4,"Hidden excluded");
    require(c.tab(1)->snapshot->entries.empty(),"Empty directory is successful");
    bool unicodeFound = false;
    for (auto& e : c.tab(0)->snapshot->entries) if (e.name == unicode) { unicodeFound = true; require(e.size == 6,"File size metadata"); }
    require(unicodeFound,"Unicode filename round trip");
    require(c.tab(0)->snapshot->entries.front().directory,"Directories sort first");
    std::vector<Entry> sorting(3); sorting[0].name = "Zoo"; sorting[1].name = "apple"; sorting[2].name = "Apple";
    sort_entries(sorting,{});
    require(sorting.back().name == "Zoo" && sorting[0].name != sorting[1].name,"Display sorting ignores ASCII case but preserves exact names");
    auto filter = cmd(Action::FilterSet); filter.options.hidden = true; c.execute(filter); settle(c);
    require(c.tab(0)->snapshot->entries.size() == 5,"Show hidden");
    auto select = cmd(Action::SelectionSet); select.snapshot = c.tab(0)->snapshot->id; select.focus = unicode; select.selection.insert(unicode); c.execute(select);
    auto edit = cmd(Action::FileEdit); edit.snapshot = select.snapshot; edit.path = unicode;
    require(c.execute(edit).launchPath == root/unicode,"Editor receives literal selected path");
    edit.action = Action::FileView;
    auto viewer = c.execute(edit);
    require(viewer.launchPath == root/unicode && viewer.viewOnly,"Viewer launch carries read-only capability request");
    auto mark = cmd(Action::SelectionPattern); mark.snapshot = c.tab(0)->snapshot->id; mark.pattern = "*.TXT"; mark.value = 1;
    c.execute(mark); require(c.tab(0)->selection.size() == 2,"Num+ additive case-insensitive wildcard selection");
    mark.pattern = "tiny.*"; mark.value = -1; c.execute(mark);
    require(c.tab(0)->selection.size() == 1 && c.tab(0)->selection.count(unicode),"Num- removes matching names only");
    mark.pattern = "*"; mark.value = 0; c.execute(mark);
    require(c.tab(0)->selection.size() == 2 && !c.tab(0)->selection.count(unicode),"Num* inversion can exclude directories");
    mark.flag = true; mark.value = 1; c.execute(mark);
    require(c.tab(0)->selection.size() == c.tab(0)->snapshot->entries.size(),"Ctrl+A includes files and directories");
    mark.value = -1; c.execute(mark); require(c.tab(0)->selection.empty(),"Unselect all");
    require(matches_pattern("README","*.*"),"Commander *.* matches extensionless names");
    require(matches_pattern("test.h","*.cpp;*.h"),"Multiple masks");
    require(matches_pattern(fs::u8path(u8"日本😀.txt"),"???.txt"),"Question mark consumes a Unicode character");
    require(!matches_pattern("test.cpp","*.h"),"Nonmatching wildcard rejected");
    mark.snapshot = 0; require(c.execute(mark).error.code == "stale_snapshot","Pattern selection rejects stale snapshots");
    c.execute(select);
    c.execute(cmd(Action::Refresh)); settle(c);
    require(c.tab(0)->selection.count(unicode) == 1,"Selection survives refresh");
    auto nav = cmd(Action::Navigate); nav.path = root/"folder"; c.execute(nav); settle(c); c.execute(cmd(Action::Parent)); settle(c);
    require(c.tab(0)->focus == "folder","Parent focuses folder just left");
    auto snapshot = c.tab(0)->snapshot;
    nav.path = root/"missing"; c.execute(nav); settle(c);
    require(c.tab(0)->error.code == "not_found" && c.tab(0)->snapshot == snapshot,"Missing path preserves listing");
    nav.path = root/"tiny.txt"; c.execute(nav); settle(c);
    require(c.tab(0)->error.code == "invalid_path","File cannot become directory location");
    auto provider = local_provider(); auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto limited = provider->list(root,false,cancel,1);
    require(limited.error.code == "resource_limit" && limited.entries.empty(),"Resource limit isn't silent truncation");
    cancel->store(true); require(provider->list(root,false,cancel,100000).error.code == "cancelled","Enumeration cancellation");
    auto longDir = root;
    for (int i = 0; i < 8; ++i) longDir /= "long-directory-component-0123456789";
    auto nativeLong = longDir;
#ifdef _WIN32
    nativeLong = fs::path(L"\\\\?\\"+longDir.native());
#endif
    fs::create_directories(nativeLong); { std::ofstream f(nativeLong/"end.txt"); f << "end"; }
    nav.path = longDir; c.execute(nav); settle(c); require(!c.tab(0)->error && c.tab(0)->snapshot->entries.size() == 1,"Long path listing");
    std::error_code ec;
    fs::create_directory_symlink(root,root/"folder"/"loop",ec);
    if (!ec) {
        nav.path = root/"folder"; c.execute(nav); settle(c);
        bool found = false; for (auto& e : c.tab(0)->snapshot->entries) if (e.name == "loop") found = e.directory && e.link;
        require(found,"Directory symlink identified");
        Id node = 0; for (const auto& kv : c.tab(0)->tree) if (kv.second.path == root/"folder") node = kv.first;
        auto expand = cmd(Action::TreeExpand); expand.node = node; c.execute(expand); settle(c);
        Id link = 0; for (auto& kv : c.tab(0)->tree) if (kv.second.link) link = kv.first;
        expand.node = link; require(c.execute(expand).error.code == "unsupported","Tree never recursively expands link cycles");
    } else std::cout << "SKIP directory symlink creation: " << ec.message() << '\n';
    auto bookmark = cmd(Action::BookmarkAdd);
    require(!c.execute(bookmark).error && c.state().bookmarks.size() == 1,"Bookmark current folder");
    c.execute(bookmark);
    require(c.state().bookmarks.size() == 1,"Duplicate bookmark suppressed");
    auto saved = c.save(); saved.width = 1300;
    auto statePath = root/"state.json";
    require(!save_state(statePath,saved),"Save session");
    auto loaded = load_state(statePath); if (loaded.error) std::cerr << "State error: " << loaded.error.message << '\n'; require(loaded.found && !loaded.error,"Load session");
    require(loaded.state.panels[0].tabs[0].path == c.tab(0)->path,"Saved native path round trip");
    Core restored; restored.restore(loaded.state); settle(restored);
    require(restored.state().bookmarks == c.state().bookmarks,"Bookmarks survive disk and core restore");
    bookmark.action = Action::BookmarkRemove; bookmark.path = c.tab(0)->path;
    restored.execute(bookmark);
    require(restored.state().bookmarks.empty(),"Remove bookmark");
    require(restored.tab(0)->path == c.tab(0)->path && !restored.tab(0)->error,"Restore active tab");
    require(!save_state(statePath,saved) && fs::exists(root/"state.json.bak"),"Backup valid session");
    { std::ofstream f(statePath); f << "{broken"; }
    require(static_cast<bool>(load_state(statePath).error),"Corrupt session diagnosed");
    require(static_cast<bool>(save_state(statePath,saved)),"Corrupt original not overwritten");
    require(fs::file_size(statePath) == 7,"Corrupt bytes preserved");
}
int main() {
    fs::path root = fs::temp_directory_path()/("aicmd-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        races(); real_fs(root);
        // Only remove our generated, resolved temporary fixture, never an input path.
        if (root.parent_path() == fs::temp_directory_path() && root.filename().string().rfind("aicmd-test-",0) == 0) {
#ifdef _WIN32
            fs::remove_all(fs::path(L"\\\\?\\"+root.native()));
#else
            fs::remove_all(root);
#endif
        }
        std::cout << "PASS " << checks << " checks\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\nFixture retained at " << root << '\n'; return 1; }
}
