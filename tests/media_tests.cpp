#include "media/session.h"
#include "fs/read_source.h"
#include <fstream>
#include <iostream>
int main() {
    using namespace av; int checks = 0;
    auto check = [&](bool ok){if (!ok) throw std::runtime_error("Media check failed at "+std::to_string(checks+1)); ++checks;};
    try {
        check(kind("A.JPEG") == Kind::Image); check(kind("x.mp4") == Kind::Video); check(kind("x.wav") == Kind::Audio); check(kind("x.txt") == Kind::Unknown);
        Session s; s.replace({{ac::Location("a.jpg"),Kind::Image},{ac::Location("b.mp4"),Kind::Video},{ac::Location("c.wav"),Kind::Audio}},0);
        check(!s.navigate({Action::Previous})); check(s.navigate({Action::Next})); auto generation = s.generation;
        s.validate({Action::StepFrame,-1,generation}); check(s.index == 1);
        check(s.navigate({Action::Last})); check(!s.navigate({Action::Next}));
        auto rejects = [&](Command c){try {s.validate(c);} catch (...) {check(true); return;} check(false);};
        rejects({Action::StepFrame,1}); rejects({Action::CopyFrame}); rejects({Action::Pause,0,generation}); rejects({Action::Select,-1}); rejects({Action::Select,0.5});
        check(s.navigate({Action::First})); rejects({Action::SeekRelative,5}); rejects({Action::Volume,101});
        auto file = ac::fs::temp_directory_path()/("aiview-read-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        {std::ofstream out(file,std::ios::binary); out << "0123456789abcdefghij";}
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {auto source = ac::open_read(ac::Location(file),cancel); check(source->size() == 20); check(source->read_at(15,100) == "fghij"); check(source->read_at(2,3) == "234"); check(source->read_at(20,5).empty()); cancel->store(true); bool cancelled = false; try {source->read_at(0,1);} catch (...) {cancelled = true;} check(cancelled);}
        ac::fs::remove(file);
        std::cout << "PASS " << checks << " media checks\n"; return 0;
    } catch (const std::exception& e) {std::cerr << e.what() << '\n'; return 1;}
}
