#include "fs/archive.h"
#include "fs/ssh.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
using namespace ac;
static int checks = 0;
static void require(bool ok,const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
template<class F> static bool fails(F f) { try { f(); return false; } catch (...) { return true; } }
static std::string contents(const fs::path& p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}}; }
static void write(const fs::path& p,const std::string& s) { std::ofstream f(p,std::ios::binary); f << s; }
static std::string repeat(const std::string& s,int n) { std::string out; while (n--) out += s; return out; }
int main() {
    const fs::path fixtures = fs::u8path(AICMD_FIXTURES);
    auto root = fs::temp_directory_path()/("aicmd-archive-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto provider = filesystem_provider();
    auto names = [&](const Location& at) {
        auto l = provider->list(at,false,cancel,64*1024*1024);
        if (l.error) throw std::runtime_error("list failed: "+l.error.message);
        std::set<std::string> out; for (auto& e : l.entries) out.insert(e.name.u8string()+(e.directory ? "/" : "")+(e.link ? "@" : ""));
        return out;
    };
    auto extract = [&](const Location& member) {
        static int n = 0; auto target = root/("x"+std::to_string(++n)); copy_location(member,target,cancel); return contents(target);
    };
    try {
        fs::create_directories(root);
        // Decoder vectors produced by zlib: stored, fixed and dynamic blocks.
        auto vectors = contents(fixtures/"deflate.bin"); size_t at = 0; int count = 0;
        while (at+8 <= vectors.size()) {
            uint32_t size = 0, packed = 0, crc = 0;
            std::memcpy(&size,vectors.data()+at,4); std::memcpy(&packed,vectors.data()+at+4,4);
            auto out = inflate_raw(vectors.substr(at+8,packed)); std::memcpy(&crc,vectors.data()+at+8+packed,4);
            require(out.size() == size && crc32(0,out.data(),out.size()) == crc,"Raw deflate vector decodes exactly"); at += 12+packed; ++count;
        }
        require(count == 3,"Three deflate vectors");
        require(gunzip(contents(fixtures/"multi.tgz")) == contents(fixtures/"sample-gnu.tar"),"Concatenated gzip members");
        require(fails([&]{ gunzip(contents(fixtures/"multi.tgz").substr(0,1000)); }),"Truncated gzip is an error");

        // ZIP browsing.
        Location zip = fixtures/"sample.zip";
        auto top = names(zip);
        require(top == std::set<std::string>({"readme.txt","stored.bin","docs/","deep/",u8"日本語/","empty.txt","zip64.txt"}),"ZIP root listing: implicit folders, UTF-8, zip-slip entry dropped");
        require(provider->list(zip,false,cancel,1<<20).readOnly,"Archive listings are read-only");
        require(names(zip/"deep"/"a"/"b") == std::set<std::string>({"c.txt"}),"Nested implicit folders");
        require(provider->list(zip/"readme.txt",false,cancel,1<<20).error.code == "invalid_path","A member file is not a folder");
        require(static_cast<bool>(provider->list(zip/"missing",false,cancel,1<<20).error),"Missing inner folder is an error");
        require(extract(zip/"readme.txt") == repeat("hello from a zip\n",20),"Deflated member");
        std::string bytes; for (int r = 0; r < 4; ++r) for (int i = 0; i < 256; ++i) bytes += static_cast<char>(i);
        require(extract(zip/"stored.bin") == bytes,"Stored member");
        require(extract(zip/"empty.txt").empty() && extract(zip/"zip64.txt") == "zip64 entry\n","Empty and ZIP64 members");
        require(extract(zip/fs::u8path(u8"日本語")/fs::u8path(u8"名前.txt")) == u8"日本語","UTF-8 member path");
        copy_location(zip/"docs",root/"docs",cancel); require(contents(root/"docs"/"guide.md") == "# Guide\n","Recursive folder extraction");
        require(names(fixtures/"cp437.zip") == std::set<std::string>({u8"cp437-ü.txt"}),"CP437 names decode");
        Location big = fixtures/"big.zip";
        auto l1 = extract(big/"level1.txt"), l6 = extract(big/"level6.txt"), l9 = extract(big/"level9.txt");
        require(l1.size() == 400000 && l1 == l6 && l1 == l9,"Large deflate members at levels 1/6/9 (CRC verified)");
        require(extract(big/"random.bin").size() == 40000,"Incompressible member");
        auto view = stage_view(zip/"readme.txt",cancel);
        require(contents(view) == repeat("hello from a zip\n",20),"F3 stages a member for viewing");

        // TAR, pax, gzip, concatenated gzip.
        for (auto name : {"sample-gnu.tar","sample-pax.tar","sample.tar.gz","multi.tgz"}) {
            Location tar = fixtures/name;
            require(names(tar) == std::set<std::string>({"dir/","long/",u8"über/","big.txt","last.txt"}),"TAR root listing");
            require(names(tar/"dir") == std::set<std::string>({"one.txt","link@"}),"Folder and symlink entries");
            require(extract(tar/"long"/std::string(120,'x')/"file.txt") == "long name\n","Long names (GNU L / pax path)");
            require(extract(tar/fs::u8path(u8"über")/fs::u8path(u8"日本.txt")) == u8"日本","Non-ASCII member");
            require(extract(tar/"last.txt") == "last" && extract(tar/"big.txt") == l1.substr(0,100000),"Member data at offsets");
            require(fails([&]{ extract(tar/"dir"/"link"); }),"Links are not extracted");
        }

        // Corruption and lookalikes.
        auto damaged = contents(fixtures/"big.zip"); damaged[damaged.size()/3] ^= 0x55; damaged[damaged.size()/3+1] ^= 0x55; write(root/"damaged.zip",damaged);
        require(fails([&]{ extract(Location(root/"damaged.zip")/"level1.txt"); }) || fails([&]{ extract(Location(root/"damaged.zip")/"level6.txt"); }),"Corrupt deflate data is detected");
        write(root/"truncated.zip",contents(fixtures/"sample.zip").substr(0,700));
        require(static_cast<bool>(provider->list(root/"truncated.zip",false,cancel,1<<20).error),"Truncated ZIP is an error, not a crash");
        fs::create_directories(root/"folder.zip"/"inside"); require(names(root/"folder.zip") == std::set<std::string>({"inside/"}),"A folder named like an archive stays a folder");

        // Core: enter, parent focus, read-only refusals, staged view.
        fs::create_directories(root/"plain"); fs::copy_file(fixtures/"sample.zip",root/"plain"/"sample.zip");
        Core core; core.start(root/"plain"/"sample.zip",root/"plain");
        auto settle = [&]{ auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30); while (core.busy()) { core.poll(); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); } core.poll(); };
        settle();
        require(core.tab(0)->snapshot && core.tab(0)->snapshot->readOnly && core.tab(0)->snapshot->entries.size() == 7,"Core opens the archive as a folder");
        Command c; c.panel = 0; c.snapshot = core.tab(0)->snapshot->id;
        c.action = Action::DirCreate; c.pattern = "x"; require(core.execute(c).error.code == "read_only","No folder creation inside archives");
        c.action = Action::FileDelete; c.path = "readme.txt"; require(core.execute(c).error.code == "read_only","No deletes inside archives");
        c.action = Action::FileCopy; auto copied = core.execute(c); settle();
        require(!copied.error && core.job(copied.job)->state == JobState::Succeeded && contents(root/"plain"/"readme.txt") == repeat("hello from a zip\n",20),"F5 extracts to the other panel");
        Command back; back.action = Action::FileCopy; back.panel = 1; back.snapshot = core.tab(1)->snapshot->id; back.path = "readme.txt";
        require(core.execute(back).error.code == "read_only","No copying into archives");
        c.action = Action::FileView; c.snapshot = core.tab(0)->snapshot->id; auto viewed = core.execute(c); settle();
        require(!viewed.error && viewed.launchPath.empty() && core.take_viewers().size() == 1,"F3 inside an archive stages a read-only copy");
        Command parent; parent.action = Action::Parent; parent.panel = 0; core.execute(parent); settle();
        require(core.tab(0)->path == Location(root/"plain") && core.tab(0)->focus == "sample.zip" && !core.tab(0)->snapshot->readOnly,"Parent leaves the archive with the cursor on it");
        std::error_code ec; fs::remove_all(root,ec);
        std::cout << "PASS " << checks << " archive checks\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << " (fixtures " << root << ")\n"; return 1; }
}
