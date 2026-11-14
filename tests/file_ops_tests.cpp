#include "fs/ssh.h"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace ac;
static int checks = 0;
static void require(bool ok,const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> static bool fails(F f) { try { f(); return false; } catch (...) { return true; } }
static std::string contents(const fs::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void write(const fs::path& p,const std::string& s) { std::ofstream f(p,std::ios::binary); f << s; if (!f) throw std::runtime_error("Fixture write failed"); }
static void settle(Core& core) {
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while (core.busy()) { core.poll(); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Core job timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    core.poll();
}
static bool listed(const Core& core,int panel,const char* name) {
    for (const auto& e : core.tab(panel)->snapshot->entries) if (e.name == fs::u8path(name)) return true;
    return false;
}
int main() {
    auto root = fs::temp_directory_path()/("aicmd-fileops-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    try {
        fs::create_directories(root/"a"); fs::create_directories(root/"b");
        // Filesystem layer.
        require(fails([&]{validate_name(root,"");}) && fails([&]{validate_name(root,"..");}) && fails([&]{validate_name(root,"x/y");}),"Invalid names rejected");
#ifdef _WIN32
        require(fails([&]{validate_name(root,"bad:name");}) && fails([&]{validate_name(root,"NUL");}) && fails([&]{validate_name(root,"trail.");}),"Windows-invalid names rejected");
#endif
        require(!fails([&]{validate_name(Location::ssh("h","/x"),"bad:name");}),"Remote names follow POSIX rules");
        make_directory(root/"a"/"new",cancel); require(fs::is_directory(root/"a"/"new"),"Create folder");
        require(fails([&]{make_directory(root/"a"/"new",cancel);}),"Existing folder is an error");
        write(root/"a"/"one.txt","one"); write(root/"a"/"two.txt","two");
        require(rename_location(root/"a"/"one.txt",root/"a"/"renamed.txt",cancel) && contents(root/"a"/"renamed.txt") == "one","Rename file");
        require(fails([&]{rename_location(root/"a"/"renamed.txt",root/"a"/"two.txt",cancel);}) && contents(root/"a"/"two.txt") == "two" && fs::exists(root/"a"/"renamed.txt"),"Rename never overwrites");
        require(rename_location(root/"a"/"renamed.txt",root/"a"/"RENAMED.txt",cancel),"Case-only rename");
        require(!rename_location(root/"a"/"RENAMED.txt",Location::ssh("h","/x"),cancel),"Cross-host rename requests copy");
        fs::create_directories(root/"a"/"tree"/"deep"); write(root/"a"/"tree"/"deep"/"f",u8"日本語");
        move_location(root/"a"/"tree",root/"b"/"tree",cancel);
        require(!fs::exists(root/"a"/"tree") && contents(root/"b"/"tree"/"deep"/"f") == u8"日本語","Move directory");
        require(fails([&]{move_location(root/"b"/"tree",root/"b"/"tree"/"deep"/"inside",cancel);}) && fs::exists(root/"b"/"tree"/"deep"/"f"),"Move into itself rejected");
        write(root/"b"/"two.txt","other");
        require(fails([&]{move_location(root/"a"/"two.txt",root/"b"/"two.txt",cancel);}) && contents(root/"a"/"two.txt") == "two" && contents(root/"b"/"two.txt") == "other","Move never overwrites and keeps source");
        fs::create_directories(root/"del"/"x"/"y"); write(root/"del"/"x"/"y"/"z","z"); write(root/"del"/"x"/"ro","ro");
        fs::permissions(root/"del"/"x"/"ro",fs::perms::owner_write,fs::perm_options::remove);
        size_t removed = 0; delete_location(root/"del"/"x",false,cancel,[&]{++removed;});
        require(!fs::exists(root/"del"/"x") && removed == 4,"Permanent recursive delete including read-only file");
        fs::create_directories(root/"del"/"target"); write(root/"del"/"target"/"keep","keep");
        std::error_code linkError; fs::create_directory_symlink(root/"del"/"target",root/"del"/"link",linkError);
        if (!linkError) { delete_location(root/"del"/"link",false,cancel); require(!fs::exists(fs::symlink_status(root/"del"/"link")) && contents(root/"del"/"target"/"keep") == "keep","Deleting a directory link keeps the target"); }
        else std::cout << "SKIP directory symlink (" << linkError.message() << ")\n";
        require(fails([&]{delete_location(root.root_path(),false,cancel);}),"Refuse deleting a root");
        cancel->store(true); require(fails([&]{delete_location(root/"del"/"target",false,cancel);}) && fs::exists(root/"del"/"target"/"keep"),"Pre-cancelled delete removes nothing"); cancel->store(false);
#ifdef _WIN32
        write(root/"del"/"trash.txt","trash"); delete_location(root/"del"/"trash.txt",true,cancel);
        require(!fs::exists(root/"del"/"trash.txt"),"Recycle Bin delete");
#else
        require(fails([&]{delete_location(root/"del"/"target",true,cancel);}),"Trash unsupported on POSIX is reported");
#endif
        // Core commands and automatic refresh.
        fs::create_directories(root/"left"); fs::create_directories(root/"right");
        for (auto name : {"m1","m2","keep","ren","gone1","gone2"}) write(root/"left"/name,name);
        Core core; core.start(root/"left",root/"right"); settle(core);
        auto snap = [&]{ return core.tab(0)->snapshot->id; };
        Command c; c.panel = 0;
        c.action = Action::DirCreate; c.snapshot = snap(); c.pattern = "made"; auto r = core.execute(c); settle(core);
        require(!r.error && core.job(r.job)->state == JobState::Succeeded && listed(core,0,"made") && core.tab(0)->focus == "made","Core mkdir refreshes and focuses the new folder");
        c.snapshot = snap(); c.pattern = "bad/name"; require(core.execute(c).error.code == "invalid_argument","Core rejects invalid folder names");
        c.action = Action::FileRename; c.snapshot = snap(); c.path = "ren"; c.pattern = "renamed"; r = core.execute(c); settle(core);
        require(!r.error && listed(core,0,"renamed") && !listed(core,0,"ren") && core.tab(0)->focus == "renamed","Core rename refreshes and focuses");
        c.snapshot = snap(); c.path = "renamed"; c.pattern = "keep"; r = core.execute(c); settle(core);
        require(!r.error && core.job(r.job)->state == JobState::Failed && contents(root/"left"/"keep") == "keep","Core rename conflict fails without overwrite");
        Command mark; mark.action = Action::SelectionPattern; mark.panel = 0; mark.snapshot = snap(); mark.pattern = "m*"; mark.value = 1; core.execute(mark);
        c.action = Action::FileMove; c.snapshot = snap(); c.path = "keep"; r = core.execute(c);
        require(!r.error && core.tab(0)->selection.empty(),"Move consumes marks");
        settle(core);
        require(core.job(r.job)->state == JobState::Succeeded && listed(core,1,"m1") && listed(core,1,"m2") && !listed(core,0,"m1") && listed(core,0,"keep"),"Core move uses marks and refreshes both panels");
        mark.snapshot = snap(); mark.pattern = "gone*"; core.execute(mark);
        c.action = Action::FileDelete; c.snapshot = snap(); c.flag = true; r = core.execute(c); settle(core);
        require(core.job(r.job)->state == JobState::Succeeded && core.job(r.job)->bytes->load() == 2 && !listed(core,0,"gone1") && !fs::exists(root/"left"/"gone2"),"Core permanent delete counts entries and refreshes");
        c.snapshot = 1; require(core.execute(c).error.code == "stale_snapshot","Stale snapshot rejected");
        fs::remove_all(root);
        std::cout << "PASS " << checks << " file operation checks\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << " (fixtures " << root << ")\n"; return 1; }
}
