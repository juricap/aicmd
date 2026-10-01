#include "core/core.h"
#include <fstream>
#include <iostream>
#include <chrono>
using namespace ac;
int main() {
    auto root = fs::temp_directory_path()/("aicmd-queue-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int checks = 0;
    auto check = [&](bool ok,const char* why) {if (!ok) throw std::runtime_error(why); ++checks;};
    try {
        fs::create_directories(root/"source"/"folder"); fs::create_directory(root/"dest"); fs::create_directory(root/"elsewhere");
        for (int i = 0;i < 600;++i) {std::ofstream f(root/"source"/("bulk-"+std::to_string(i))); f << std::string(4096,'x');}
        for (auto name : {"cancel","after","last"}) {std::ofstream f(root/"source"/name); f << name;}
        {
            Core core; core.start(root/"source",root/"dest");
            auto wait = [&](auto predicate) {auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(60); while (!predicate()) {core.poll(); if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Queue test timeout"); std::this_thread::sleep_for(std::chrono::milliseconds(1));}};
            wait([&]{return !core.busy();});
            Command select; select.action = Action::SelectionPattern; select.panel = 0; select.snapshot = core.tab(0)->snapshot->id; select.pattern = "bulk-*"; select.value = 1;
            check(!core.execute(select).error,"Select bulk files");
            Command copy; copy.action = Action::FileCopy; copy.panel = 0; copy.snapshot = select.snapshot;
            auto first = core.execute(copy); check(!first.error && core.job(first.job)->progress->totalEntries == 600,"Large set is one bounded queued job");
            select.value = -1; core.execute(select);
            copy.path = "cancel"; auto cancelled = core.execute(copy);
            copy.path = "after"; auto after = core.execute(copy); auto conflict = core.execute(copy);
            copy.path = "last"; auto last = core.execute(copy);
            check(!cancelled.error && !after.error && !conflict.error && !last.error,"Additional copies accepted while busy");
            Command cancel; cancel.action = Action::JobCancel; cancel.job = cancelled.job; core.execute(cancel); core.poll();
            check(core.job(cancelled.job)->state == JobState::Cancelled,"Queued cancellation acknowledged without waiting for active job");
            check(!fs::exists(root/"dest"/"cancel"),"Cancelled queued job does not create files");
            Command nav; nav.action = Action::Navigate; nav.panel = 0; nav.path = root/"source"/"folder"; core.execute(nav);
            nav.panel = 1; nav.path = root/"elsewhere"; core.execute(nav);
            uint64_t previous = 0;
            wait([&]{auto bytes = core.job(first.job)->bytes->load(); if (bytes < previous) throw std::runtime_error("Progress went backwards"); previous = bytes; return !core.busy();});
            check(core.tab(0)->path == Location(root/"source"/"folder") && core.tab(1)->path == Location(root/"elsewhere"),"Navigation remains independent of copy jobs");
            check(core.job(first.job)->state == JobState::Succeeded && core.job(first.job)->progress->completedEntries == 600 && previous == 600*4096,"Complete large-set progress");
            check(core.job(after.job)->state == JobState::Succeeded && core.job(conflict.job)->state == JobState::Failed && core.job(last.job)->state == JobState::Succeeded,"FIFO conflict fails only its job and queue continues");
            check(fs::exists(root/"dest"/"last") && fs::is_empty(root/"elsewhere"),"Destination frozen at submission");
            nav.panel = 0; nav.path = root/"source"; core.execute(nav); nav.panel = 1; nav.path = root/"elsewhere"; core.execute(nav); wait([&]{return !core.busy();});
            copy.snapshot = core.tab(0)->snapshot->id; copy.path = "bulk-0";
            select.snapshot = copy.snapshot; select.value = 1; core.execute(select);
            auto interrupted = core.execute(copy);
            wait([&]{return core.job(interrupted.job)->bytes->load() > 0 || !job_pending(core.job(interrupted.job)->state);});
            cancel.job = interrupted.job; core.execute(cancel); wait([&]{return !core.busy();});
            check(core.job(interrupted.job)->state == JobState::Cancelled,"Active copy acknowledges cancellation after worker cleanup");
            for (const auto& entry : fs::directory_iterator(root/"elsewhere")) check(entry.path().filename().u8string().find(".aicmd-part-") != 0,"No local staging file after cancellation");
        }
        fs::remove_all(root); std::cout << "PASS " << checks << " copy queue checks\n"; return 0;
    } catch (const std::exception& e) {std::cerr << "FAIL " << e.what() << " fixtures=" << root << '\n'; return 1;}
}
