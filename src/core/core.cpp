#include "core.h"
#include <algorithm>
#include <stdexcept>

namespace ac {
const std::vector<CommandInfo>& commands() {
    static const std::vector<CommandInfo> items = {
        {Action::SessionGet,"session_get","Read session state","read"},
        {Action::CapabilitiesGet,"capabilities_get","Discover commands","read"},
        {Action::PanelActivate,"panel_activate","Activate panel","session"},
        {Action::PanelConfigure,"panel_configure","Set tree visibility, width and split","session"},
        {Action::TabCreate,"tab_create","Duplicate location in a new tab","session"},
        {Action::TabActivate,"tab_activate","Activate a tab","session"},
        {Action::TabClose,"tab_close","Close a tab","session"},
        {Action::TabCycle,"tab_cycle","Cycle panel tabs","session"},
        {Action::Navigate,"navigate","Navigate to a directory","session"},
        {Action::Parent,"navigate_parent","Navigate to parent","session"},
        {Action::Back,"navigate_back","Navigate backward","session"},
        {Action::Forward,"navigate_forward","Navigate forward","session"},
        {Action::Refresh,"refresh","Refresh directory","session"},
        {Action::TreeExpand,"tree_expand","Load immediate subdirectories","session"},
        {Action::TreeCollapse,"tree_collapse","Collapse a tree node","session"},
        {Action::TreeReveal,"tree_reveal","Reveal directory ancestors","session"},
        {Action::SelectionSet,"selection_set","Set selection and focused entry","session"},
        {Action::ViewportSet,"viewport_set","Remember top visible entry","session"},
        {Action::SortSet,"sort_set","Set ordering","session"},
        {Action::FilterSet,"filter_set","Set hidden entry visibility","session"},
        {Action::JobGet,"job_get","Read a job","read"},
        {Action::JobCancel,"job_cancel","Cancel tab work","session"},
        {Action::FileEdit,"file_edit","Request host editor launch","launch"},
        {Action::FileView,"file_view","Request read-only viewer launch","launch"},
        {Action::SessionSave,"session_save","Request host persistence","host"},
        {Action::SelectionPattern,"selection_pattern","Add, remove, or invert marks matching a wildcard mask","session"},
        {Action::AppQuit,"app_quit","Quit session","session"}
    };
    return items;
}
Core::Core(std::shared_ptr<Provider> p) : provider_(std::move(p)), roots_(provider_->roots()) {
    for (int i = 0; i < 2; ++i) workers_.emplace_back([this]{ worker(); });
}
Core::~Core() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        for (auto& w : queue_) w.cancel->store(true);
        for (auto& c : running_) c->store(true);
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}
const Tab* Core::tab(int p, Id id) const {
    if (p < 0 || p > 1) return nullptr;
    const auto& panel = state_.panels[p];
    if (!id) id = panel.active;
    for (const auto& t : panel.tabs) if (t.id == id) return &t;
    return nullptr;
}
Tab* Core::mutable_tab(int p, Id id) { return const_cast<Tab*>(tab(p,id)); }
const Job* Core::job(Id id) const { auto it = jobs_.find(id); return it == jobs_.end() ? nullptr : &it->second; }
void Core::prune_jobs() {
    while (jobs_.size() > 256) {
        auto it = std::find_if(jobs_.begin(), jobs_.end(), [](const auto& j){return j.second.state != JobState::Running;});
        if (it == jobs_.end()) break;
        jobs_.erase(it);
    }
}
bool Core::submit(Work w) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.erase(std::remove_if(queue_.begin(),queue_.end(),[](const Work& q){return q.cancel->load();}),queue_.end());
    if (queue_.size() >= 64) return false;
    queue_.push_back(std::move(w));
    cv_.notify_one();
    return true;
}
void Core::worker() {
    for (;;) {
        Work w;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{return stopping_ || !queue_.empty();});
            if (stopping_) return;
            w = std::move(queue_.front()); queue_.pop_front(); running_.push_back(w.cancel);
        }
        Listing listing;
        try {
            if (w.cancel->load()) listing.error = {"cancelled","Cancelled"};
            else listing = provider_->list(w.path,w.node != 0,w.cancel,w.budget);
            if (!listing.error) {
                if (!w.options.hidden) listing.entries.erase(std::remove_if(listing.entries.begin(),listing.entries.end(),[](const Entry& e){return e.hidden || e.system;}),listing.entries.end());
                sort_entries(listing.entries,w.options);
            }
        } catch (const std::exception& e) { listing.error = {"unavailable",e.what()}; }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_.erase(std::remove(running_.begin(),running_.end(),w.cancel),running_.end());
            if (!w.cancel->load()) done_.push_back({std::move(w),std::move(listing)});
        }
    }
}
void Core::cancel(Tab& t) {
    if (t.cancel) t.cancel->store(true);
    auto j = jobs_.find(t.job);
    if (j != jobs_.end() && j->second.state == JobState::Running) j->second.state = JobState::Cancelled;
    ++t.generation;
    t.loading = false;
}
Result Core::navigate(int p, Tab& t, fs::path path, int target, bool refresh, fs::path focusAfter) {
    path = absolute_path(path,t.path);
    if (path.empty()) return {{"invalid_path","Empty location"}};
    cancel(t);
    t.requested = path; t.error = {}; t.loading = true;
    t.cancel = std::make_shared<std::atomic<bool>>(false);
    t.job = next_++;
    jobs_[t.job] = {t.job}; prune_jobs();
    Work w; w.panel = p; w.tab = t.id; w.generation = t.generation; w.job = t.job;
    w.path = path; w.focusAfter = std::move(focusAfter); w.historyTarget = target; w.refresh = refresh;
    w.options = t.options; w.cancel = t.cancel; w.budget = 32 * 1024 * 1024;
    if (!submit(std::move(w))) {
        t.loading = false; t.error = {"resource_limit","Too many pending scans. Retry after current work finishes."};
        jobs_[t.job].state = JobState::Failed; jobs_[t.job].error = t.error;
        return {t.error};
    }
    ++state_.revision;
    Result r; r.tab = t.id; r.job = t.job; return r;
}
void Core::init_tree(Tab& t) {
    for (auto& kv : t.tree) if (kv.second.cancel) kv.second.cancel->store(true);
    t.tree.clear(); t.roots.clear();
    for (auto& root : roots_) { TreeNode n; n.id = next_++; n.path = root; t.roots.push_back(n.id); t.tree.emplace(n.id,std::move(n)); }
    reveal(t,t.path);
}
Id Core::reveal(Tab& t, const fs::path& path) {
    if (path.empty()) return 0;
    std::vector<fs::path> chain;
    auto q = path;
    for (int depth = 0; depth < 256 && !q.empty(); ++depth) {
        chain.push_back(q); auto parent = q.parent_path(); if (q == parent) break; q = parent;
    }
    std::reverse(chain.begin(),chain.end());
    Id parent = 0;
    for (auto& part : chain) {
        Id found = 0;
        const auto& ids = parent ? t.tree.at(parent).children : t.roots;
        for (Id id : ids) if (t.tree.at(id).path == part) { found = id; break; }
        if (!found) {
            if (t.tree.size() >= 10000) break;
            TreeNode n; n.id = next_++; n.parent = parent; n.path = part; found = n.id;
            t.tree.emplace(n.id,std::move(n));
            if (parent) t.tree.at(parent).children.push_back(found); else t.roots.push_back(found);
        }
        if (parent) t.tree.at(parent).expanded = true;
        parent = found;
    }
    return parent;
}
Result Core::expand(int p, Tab& t, Id id) {
    auto it = t.tree.find(id);
    if (it == t.tree.end()) return {{"not_found","Tree node no longer exists"}};
    auto& n = it->second;
    n.expanded = true;
    if (n.link) { n.error = {"unsupported","Browse linked folders from the file list; tree expansion does not follow links."}; return {n.error}; }
    if (n.load == Load::Loaded || n.load == Load::Loading) return {};
    n.cancel = std::make_shared<std::atomic<bool>>(false); ++n.generation; n.load = Load::Loading; n.error = {};
    Work w; w.panel = p; w.tab = t.id; w.node = id; w.generation = n.generation; w.path = n.path;
    w.options.hidden = t.options.hidden; w.cancel = n.cancel; w.budget = 2 * 1024 * 1024;
    if (!submit(std::move(w))) { n.load = Load::Failed; n.error = {"resource_limit","Scan queue is full; collapse and expand to retry."}; return {n.error}; }
    ++state_.revision;
    return {};
}
bool Core::poll() {
    std::deque<Done> done;
    { std::lock_guard<std::mutex> lock(mutex_); done.swap(done_); }
    bool changed = false;
    for (auto& d : done) {
        auto& w = d.work;
        Tab* t = mutable_tab(w.panel,w.tab);
        if (!t || w.cancel->load()) continue;
        if (w.node) {
            auto it = t->tree.find(w.node);
            if (it == t->tree.end() || it->second.generation != w.generation) continue;
            auto& n = it->second;
            if (t->tree.size() + d.listing.entries.size() > 10000) d.listing.error = {"resource_limit","Tree node limit reached"};
            n.error = d.listing.error;
            n.load = n.error ? Load::Failed : Load::Loaded;
            if (!n.error) {
                std::vector<Id> children;
                std::map<fs::path,Id> oldChildren;
                for (Id old : n.children) oldChildren.emplace(t->tree.at(old).path,old);
                for (auto& e : d.listing.entries) {
                    fs::path childPath = n.path / e.name; Id child = 0;
                    auto old = oldChildren.find(childPath); if (old != oldChildren.end()) child = old->second;
                    if (!child) { TreeNode c; c.id = next_++; c.parent = n.id; c.path = childPath; c.link = e.link; child = c.id; t->tree.emplace(c.id,std::move(c)); }
                    else t->tree.at(child).link = e.link;
                    children.push_back(child);
                }
                n.children = std::move(children);
            }
        } else {
            if (t->generation != w.generation) continue;
            t->loading = false; t->error = d.listing.error;
            auto& j = jobs_[w.job];
            if (t->error) { j.state = JobState::Failed; j.error = t->error; }
            else {
                // Bound aggregate retained list data; inactive tabs are the first eviction candidates.
                size_t used = d.listing.bytes;
                for (auto& panel : state_.panels) for (auto& other : panel.tabs)
                    if (&other != t && other.snapshot) used += other.snapshot->bytes;
                for (auto& panel : state_.panels) for (auto& other : panel.tabs)
                    if (used > 128 * 1024 * 1024 && &other != t && other.id != panel.active && other.snapshot) { used -= other.snapshot->bytes; other.snapshot.reset(); }
                auto snap = std::make_shared<Snapshot>(); snap->id = next_++; snap->path = w.path;
                snap->entries = std::move(d.listing.entries); snap->bytes = d.listing.bytes;
                bool same = t->path == w.path;
                t->path = w.path;
                if (!same) { t->selection.clear(); t->focus.clear(); t->anchor.clear(); }
                if (!w.focusAfter.empty()) t->focus = w.focusAfter;
                if (w.historyTarget >= 0) t->historyIndex = static_cast<size_t>(w.historyTarget);
                else if (!w.refresh && (t->history.empty() || t->history[t->historyIndex] != t->path)) {
                    if (!t->history.empty()) t->history.resize(t->historyIndex + 1);
                    t->history.push_back(t->path);
                    if (t->history.size() > 100) t->history.erase(t->history.begin());
                    t->historyIndex = t->history.size() - 1;
                }
                std::set<fs::path> available;
                for (const auto& e : snap->entries) available.insert(e.name);
                for (auto it = t->selection.begin(); it != t->selection.end();) if (!available.count(*it)) it = t->selection.erase(it); else ++it;
                if (!available.count(t->focus)) t->focus = snap->entries.empty() ? fs::path{} : snap->entries.front().name;
                t->snapshot = snap;
                reveal(*t,t->path);
                j.state = JobState::Succeeded;
            }
        }
        changed = true;
    }
    if (changed) ++state_.revision;
    return changed;
}
Result Core::execute(const Command& c) {
    if (c.expectedRevision && *c.expectedRevision != state_.revision) return {{"revision_conflict","Session changed"}};
    if (c.panel < 0 || c.panel > 1) return {{"invalid_argument","Panel must be 0 or 1"}};
    auto& panel = state_.panels[c.panel];
    Tab* t = mutable_tab(c.panel,c.tab);
    Result r;
    switch (c.action) {
    case Action::SessionGet: case Action::CapabilitiesGet: return {};
    case Action::JobGet: return job(c.job) ? Result{} : Result{{"not_found","Job expired or unknown"}};
    case Action::SessionSave: r.save = true; return r;
    case Action::AppQuit: state_.quit = true; break;
    case Action::PanelActivate: state_.activePanel = c.panel; break;
    case Action::PanelConfigure:
        panel.treeVisible = c.flag; panel.treeWidth = std::clamp(c.value,80,600);
        if (c.node) state_.splitPercent = std::clamp(static_cast<int>(c.node),20,80);
        break;
    case Action::TabCreate: {
        if (panel.tabs.size() >= 32) return {{"resource_limit","Maximum 32 tabs per panel"}};
        Tab fresh; fresh.id = next_++;
        fresh.path = !c.path.empty() ? c.path : t ? t->path : fs::current_path();
        if (t) fresh.options = t->options;
        init_tree(fresh); panel.active = fresh.id; panel.tabs.push_back(std::move(fresh));
        return navigate(c.panel,panel.tabs.back(),panel.tabs.back().path);
    }
    default:
        if (!t) return {{"not_found","Tab no longer exists"}};
        switch (c.action) {
        case Action::TabActivate:
            panel.active = t->id;
            if (!t->snapshot && !t->loading) return navigate(c.panel,*t,t->path,-1,true);
            break;
        case Action::TabCycle: {
            size_t index = 0;
            for (; index < panel.tabs.size() && panel.tabs[index].id != panel.active; ++index) {}
            index = (index + panel.tabs.size() + (c.value < 0 ? -1 : 1)) % panel.tabs.size();
            panel.active = panel.tabs[index].id;
            auto& active = panel.tabs[index];
            if (!active.snapshot && !active.loading) return navigate(c.panel,active,active.path,-1,true);
            break;
        }
        case Action::TabClose: {
            if (panel.tabs.size() == 1) return {{"last_tab","Each panel needs at least one tab"}};
            Id id = t->id; cancel(*t);
            for (auto& kv : t->tree) if (kv.second.cancel) kv.second.cancel->store(true);
            size_t index = 0; while (panel.tabs[index].id != id) ++index;
            panel.tabs.erase(panel.tabs.begin()+index);
            if (panel.active == id) panel.active = panel.tabs[std::min(index,panel.tabs.size()-1)].id;
            auto& active = *mutable_tab(c.panel);
            if (!active.snapshot && !active.loading) return navigate(c.panel,active,active.path,-1,true);
            break;
        }
        case Action::Navigate: return navigate(c.panel,*t,c.path);
        case Action::Parent: return navigate(c.panel,*t,t->path.parent_path(),-1,false,t->path.filename());
        case Action::Back:
            if (!t->history.empty() && t->historyIndex) return navigate(c.panel,*t,t->history[t->historyIndex-1],static_cast<int>(t->historyIndex-1));
            return {};
        case Action::Forward:
            if (t->historyIndex+1 < t->history.size()) return navigate(c.panel,*t,t->history[t->historyIndex+1],static_cast<int>(t->historyIndex+1));
            return {};
        case Action::Refresh:
            init_tree(*t);
            return navigate(c.panel,*t,t->error && !t->requested.empty() ? t->requested : t->path,-1,true);
        case Action::TreeExpand: return expand(c.panel,*t,c.node);
        case Action::TreeCollapse: {
            auto it = t->tree.find(c.node); if (it == t->tree.end()) return {{"not_found","Unknown tree node"}};
            it->second.expanded = false; break;
        }
        case Action::TreeReveal: reveal(*t,c.path.empty() ? t->path : c.path); break;
        case Action::SelectionSet: {
            if (!t->snapshot || t->snapshot->id != c.snapshot) return {{"stale_snapshot","Selection snapshot has changed"}};
            size_t found = 0; bool focusFound = c.focus.empty();
            for (const auto& e : t->snapshot->entries) {
                if (c.selection.count(e.name)) ++found;
                if (e.name == c.focus) focusFound = true;
            }
            if (found != c.selection.size()) return {{"not_found","Selection entry is missing"}};
            if (!focusFound) return {{"not_found","Focused entry is missing"}};
            t->selection = c.selection; t->focus = c.focus; break;
        }
        case Action::SelectionPattern: {
            if (!t->snapshot || t->snapshot->id != c.snapshot) return {{"stale_snapshot","Selection snapshot has changed"}};
            if (c.pattern.empty() || c.pattern.size() > 1024 || c.pattern.find('\0') != std::string::npos || c.value < -1 || c.value > 1) return {{"invalid_argument","Invalid selection pattern/mode"}};
            auto selected = t->selection;
            try {
                for (const auto& e : t->snapshot->entries) {
                    if ((e.directory && !c.flag) || !matches_pattern(e.name,c.pattern)) continue;
                    if (c.value == -1 || (c.value == 0 && selected.count(e.name))) selected.erase(e.name);
                    else selected.insert(e.name);
                }
            } catch (const std::exception&) { return {{"invalid_argument","Invalid selection pattern encoding"}}; }
            t->selection = std::move(selected); break;
        }
        case Action::ViewportSet: t->anchor = c.anchor; break;
        case Action::SortSet: case Action::FilterSet:
            t->options = c.options; if (c.action == Action::FilterSet) init_tree(*t);
            return navigate(c.panel,*t,t->path,-1,true);
        case Action::JobCancel:
            cancel(*t); t->error = {"cancelled","Navigation cancelled"};
            for (auto& kv : t->tree) if (kv.second.load == Load::Loading) {
                kv.second.cancel->store(true); kv.second.load = Load::Unloaded;
            }
            break;
        case Action::FileView: case Action::FileEdit:
            if (!t->snapshot || c.snapshot != t->snapshot->id) return {{"stale_snapshot","File listing changed"}};
            for (const auto& e : t->snapshot->entries) if (e.name == c.path && !e.directory) { r.launchPath = t->path / e.name; r.viewOnly = c.action == Action::FileView; return r; }
            return {{"not_found","Select a file to edit"}};
        default: return {{"unsupported","Command not implemented"}};
        }
    }
    ++state_.revision;
    return r;
}
void Core::start(const fs::path& left, const fs::path& right) {
    for (int p = 0; p < 2; ++p) { Command c; c.action = Action::TabCreate; c.panel = p; c.path = p ? right : left; execute(c); }
}
bool Core::busy() const {
    for (const auto& p : state_.panels) for (const auto& t : p.tabs) {
        if (t.loading) return true;
        for (const auto& kv : t.tree) if (kv.second.load == Load::Loading) return true;
    }
    return false;
}
SavedState Core::save() const {
    SavedState out; out.activePanel = state_.activePanel; out.splitPercent = state_.splitPercent;
    for (int p = 0; p < 2; ++p) {
        auto& dest = out.panels[p]; const auto& panel = state_.panels[p];
        dest.treeVisible = panel.treeVisible; dest.treeWidth = panel.treeWidth;
        for (const auto& t : panel.tabs) {
            if (t.id == panel.active) dest.active = dest.tabs.size();
            SavedTab s; s.path = t.path; s.focus = t.focus; s.anchor = t.anchor; s.options = t.options;
            s.history = t.history; s.historyIndex = t.historyIndex;
            for (auto& kv : t.tree) if (kv.second.expanded && s.expanded.size() < 256) s.expanded.push_back(kv.second.path);
            dest.tabs.push_back(std::move(s));
        }
    }
    return out;
}
void Core::restore(const SavedState& saved) {
    for (auto& p : state_.panels) for (auto& t : p.tabs) { cancel(t); for (auto& kv : t.tree) if (kv.second.cancel) kv.second.cancel->store(true); }
    state_.panels = {};
    state_.activePanel = std::clamp(saved.activePanel,0,1); state_.splitPercent = std::clamp(saved.splitPercent,20,80);
    for (int p = 0; p < 2; ++p) {
        const auto& src = saved.panels[p]; auto& panel = state_.panels[p];
        panel.treeVisible = src.treeVisible; panel.treeWidth = std::clamp(src.treeWidth,80,600);
        for (const auto& s : src.tabs) {
            if (panel.tabs.size() >= 32) break;
            Tab t; t.id = next_++; t.path = s.path; t.options = s.options; t.history = s.history;
            if (t.history.size() > 100) t.history.resize(100);
            t.historyIndex = t.history.empty() ? 0 : std::min(s.historyIndex,t.history.size()-1);
            t.focus = s.focus; t.anchor = s.anchor; init_tree(t);
            for (const auto& path : s.expanded) { auto id = reveal(t,path); if (id) t.tree.at(id).expanded = true; }
            panel.tabs.push_back(std::move(t));
        }
        if (panel.tabs.empty()) { Command c; c.action = Action::TabCreate; c.panel = p; c.path = fs::current_path(); execute(c); }
        panel.active = panel.tabs[std::min(src.active,panel.tabs.size()-1)].id;
        // Only active tabs are scanned on restore. Other tabs load on activation.
        auto& active = *mutable_tab(p); navigate(p,active,active.path,-1,true);
    }
    ++state_.revision;
}
}
