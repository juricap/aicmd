#include "fs/ssh.h"
#include "host/state.h"
#include <fstream>
#include <iostream>
#include <chrono>
using namespace ac;
static int checks = 0;
static void require(bool ok,const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> static bool fails(F f) { try { f(); return false; } catch (...) { return true; } }
static std::string contents(const fs::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void write(const fs::path& p,const std::string& s) { std::ofstream f(p,std::ios::binary); f.write(s.data(),s.size()); if (!f) throw std::runtime_error("Fixture write failed"); }
static void settle(Core& core) {
    auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while (core.busy()) { core.poll(); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Core job timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}
int main(int argc,char** argv) {
    auto root = fs::temp_directory_path()/("aicmd-ssh-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root/"conf.d");
        write(root/"config","Host alpha beta # comment\nHost *.invalid !excluded alpha\nInclude conf.d/*\nHost \"quoted\"\nHost=equal\n");
        write(root/"conf.d"/"one","Host included\nInclude config\n");
        auto hosts = ssh_hosts(root/"config"); require(hosts == std::vector<std::string>({"alpha","beta","included","quoted","equal"}),"SSH aliases, Include, comments, dedup and wildcard exclusions");
        require(ssh_hosts(root/"missing").empty(),"Missing SSH config is valid");
        auto remote = Location::ssh("ai","/data/devel");
        require(Location(fs::u8path("ssh://ai/data/devel")) == remote,"Parse SSH location");
        require(remote.parent_path() == Location::ssh("ai","/data"),"Remote parent");
        require(remote.root_path().parent_path() == remote.root_path(),"Remote root cannot escape authority");
        require((remote/fs::u8path(u8"日本語 space.txt")).remote == u8"/data/devel/日本語 space.txt","Remote Unicode child");
        require(absolute_path(Location("/etc"),remote) == Location::ssh("ai","/etc"),"Absolute POSIX path stays remote");
        require(fails([]{Location::ssh("-oProxyCommand=bad");}),"SSH option injection rejected");
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        write(root/"source.txt","original"); copy_location(root/"source.txt",root/"dest.txt",cancel);
        require(contents(root/"dest.txt") == "original","Local streaming copy");
        write(root/"source.txt","changed"); require(fails([&]{copy_location(root/"source.txt",root/"dest.txt",cancel);}) && contents(root/"dest.txt") == "original","Existing file is never overwritten");
        fs::create_directory(root/"tree"); write(root/"tree"/"child","child");
        require(fails([&]{copy_location(root/"tree",root/"tree"/"nested",cancel);}),"Recursive self-copy rejected");
        cancel->store(true); require(fails([&]{copy_location(root/"source.txt",root/"cancelled",cancel);}) && !fs::exists(root/"cancelled"),"Precancelled copy creates no destination"); cancel->store(false);
        Core saved; saved.start(root,root); settle(saved); auto state = saved.save(); state.bookmarks = {remote}; state.panels[0].tabs[0].path = remote; state.panels[0].tabs[0].history = {remote};
        require(!save_state(root/"session.json",state),"Save remote location"); auto loaded = load_state(root/"session.json");
        require(!loaded.error && loaded.state.panels[0].tabs[0].path == remote && loaded.state.bookmarks[0] == remote,"Remote tab/history/bookmark roundtrip");
        if (argc == 3) {
            auto base = Location::ssh(argv[1],argv[2]);
            if (base.remote.rfind("/data/devel/aicmd-ssh-check-",0) != 0) throw std::runtime_error("Live tests require isolated /data/devel/aicmd-ssh-check-* directory");
            auto provider = filesystem_provider(); auto listing = provider->list(base,false,cancel,32*1024*1024);
            require(!listing.error,"Live SFTP directory listing");
            std::string bytes(8*1024*1024,'\0'); for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i*37+i/251)%256);
            auto filename = fs::u8path(u8"日本語 space.bin"); write(root/filename,bytes);
            uint64_t progress = 0; auto start = std::chrono::steady_clock::now();
            copy_location(root/filename,base/filename,cancel,[&](uint64_t n){require(n >= progress,"Monotonic upload progress");progress = n;});
            require(progress == bytes.size(),"Upload byte count");
            copy_location(base/filename,root/"roundtrip.bin",cancel);
            require(contents(root/"roundtrip.bin") == bytes,"8 MiB upload/download byte exact");
            std::cout << "8 MiB roundtrip seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count() << '\n';
            require(fails([&]{copy_location(root/filename,base/filename,cancel);}),"Remote existing destination rejected");
            copy_location(base/filename,base/"remote-copy.bin",cancel); copy_location(base/"remote-copy.bin",root/"remote-copy.bin",cancel);
            require(contents(root/"remote-copy.bin") == bytes,"Remote-to-remote bounded streaming");
            copy_location(root/"tree",base/"tree",cancel); copy_location(base/"tree",root/"tree-roundtrip",cancel);
            require(contents(root/"tree-roundtrip"/"child") == "child","Recursive directory upload/download");
            write(root/"empty",""); copy_location(root/"empty",base/"empty",cancel); copy_location(base/"empty",root/"empty-down",cancel);
            require(fs::file_size(root/"empty-down") == 0,"Empty file roundtrip");
            auto view = stage_view(base/filename,cancel); require(contents(view) == bytes,"Remote viewer staging");
            require(stage_view(base/filename,cancel) == view,"Unchanged remote view reuses cached stage");
            write(view,"locally edited"); require(stage_view(base/filename,cancel) != view,"Edited stage is never reused as remote content");
            require(fails([&]{copy_location(base/filename,root/"interrupted",cancel,[&](uint64_t){cancel->store(true);});}) && !fs::exists(root/"interrupted"),"Mid-transfer cancellation does not publish partial destination"); cancel->store(false);
            copy_location(base/filename,root/"after-cancel",cancel); require(contents(root/"after-cancel") == bytes,"Reconnect after cancellation");
            Core core; core.start(base,root); settle(core); require(!core.tab(0)->error,"Remote core browsing");
            Command cmd; cmd.action = Action::FileView; cmd.snapshot = core.tab(0)->snapshot->id; cmd.path = filename; auto result = core.execute(cmd); settle(core);
            require(!result.error && core.job(result.job)->state == JobState::Succeeded && core.take_viewers().size() == 1,"Asynchronous remote viewer command");
            cmd.action = Action::FileCopy; cmd.path = "empty"; result = core.execute(cmd); settle(core);
            require(!result.error && core.job(result.job)->state == JobState::Failed,"Core copy reports destination conflict");
        }
        std::cout << "PASS " << checks << " SSH/copy checks\n";
        fs::remove_all(root); return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << " (fixtures " << root << ")\n"; return 1; }
}
