#pragma once
#include "location.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ac {
namespace fs = std::filesystem;
using Id = uint64_t;
using Cancel = std::shared_ptr<std::atomic<bool>>;
struct Error {
    std::string code, message;
    int native = 0;
    explicit operator bool() const { return !code.empty(); }
};
struct Entry {
    Id id = 0;
    fs::path name;
    bool directory = false, link = false, hidden = false;
    bool readOnly = false, system = false, archive = false;
    std::optional<uint64_t> size;
    std::optional<int64_t> modified; // Unix seconds
};
enum class Sort { Name, Extension, Size, Modified };
struct Options { Sort sort = Sort::Name; bool descending = false, hidden = false; };
struct Listing { std::vector<Entry> entries; Error error; size_t bytes = 0; };
class Provider {
public:
    virtual ~Provider() = default;
    virtual std::vector<Location> roots() = 0;
    virtual Listing list(const Location&, bool directoriesOnly, const Cancel&, size_t budget) = 0;
    // Streams unsorted entries; the visitor returns false to stop early (not an error).
    // Links are reported, never resolved. lane selects an independent remote connection.
    virtual Error enumerate(const Location&, const Cancel&, const std::function<bool(Entry&&)>& visit, const char* lane = "browse");
};
std::shared_ptr<Provider> local_provider();
std::shared_ptr<Provider> filesystem_provider();
fs::path absolute_path(const fs::path&, const fs::path& base);
std::string display_path(const fs::path&);
std::string safe_text(std::string);
void sort_entries(std::vector<Entry>&, const Options&);
bool matches_pattern(const fs::path& name, const std::string& pattern);

class Search;
struct Snapshot { Id id = 0; Location path; std::vector<Entry> entries; size_t bytes = 0; };
enum class Load { Unloaded, Loading, Loaded, Failed };
struct TreeNode {
    Id id = 0, parent = 0, generation = 0;
    Location path;
    std::vector<Id> children;
    bool expanded = false, link = false;
    Load load = Load::Unloaded;
    Error error;
    Cancel cancel;
};
struct Tab {
    Id id = 0, generation = 0, job = 0;
    Location path, requested;
    std::vector<Location> history;
    size_t historyIndex = 0;
    Options options;
    std::shared_ptr<const Snapshot> snapshot;
    std::set<fs::path> selection;
    fs::path focus, anchor;
    std::map<Id, TreeNode> tree;
    std::vector<Id> roots;
    bool loading = false;
    Error error;
    Cancel cancel;
    // Search result tab: snapshot entries are paths relative to `path`, published progressively.
    std::shared_ptr<Search> search;
    std::vector<Entry> found;
    int64_t searchPublished = 0;
    bool searchFinal = false;
};
struct Panel { std::vector<Tab> tabs; Id active = 0; bool treeVisible = false; int treeWidth = 170; };
struct Session {
    std::vector<Location> bookmarks;
    Id revision = 0;
    int activePanel = 0, splitPercent = 50;
    std::array<Panel, 2> panels;
    bool quit = false;
};
// No GUI handles or JSON types cross the command boundary.
enum class Action {
    SessionGet, CapabilitiesGet, PanelActivate, PanelConfigure, TabCreate, TabActivate,
    TabClose, TabCycle, Navigate, Parent, Back, Forward, Refresh, TreeExpand, TreeCollapse,
    TreeReveal, SelectionSet, ViewportSet, SortSet, FilterSet, JobGet, JobCancel,
    FileEdit, FileView, FileCopy, FileMove, FileRename, FileDelete, DirCreate, ConnectionsGet, SessionSave, SelectionPattern, BookmarkAdd, BookmarkRemove, SearchStart, AppQuit
};
struct CommandInfo { Action action; const char* name; const char* description; const char* effect; };
const std::vector<CommandInfo>& commands();
struct Command {
    Action action = Action::SessionGet;
    int panel = 0;
    Id tab = 0, node = 0, snapshot = 0, job = 0;
    Location path;
    fs::path focus, anchor;
    std::set<fs::path> selection;
    Options options;
    std::string pattern = "*";
    std::string text; // search content
    int value = 0;
    bool flag = false;
    std::optional<Id> expectedRevision;
};
struct Result { Error error; Id tab = 0, job = 0; Location launchPath{}; bool save = false, viewOnly = false; std::vector<std::string> connections{}; };
enum class JobState { Running, Succeeded, Failed, Cancelled, Queued, Cancelling };
enum class JobKind { View, Copy, Move, Rename, Delete, Mkdir };
struct TransferProgress {
    std::atomic<bool> started{false};
    std::atomic<uint64_t> completedEntries{0};
    uint64_t totalEntries = 0;
    std::mutex mutex;
    Location current;
};
inline bool job_pending(JobState s) { return s == JobState::Running || s == JobState::Queued || s == JobState::Cancelling; }
// bytes counts transferred bytes, or removed entries for Delete. source is the directory the entries came from.
struct Job { Id id = 0; JobState state = JobState::Running; JobKind kind = JobKind::View; Error error{}; Cancel cancel; std::shared_ptr<std::atomic<uint64_t>> bytes; std::shared_ptr<TransferProgress> progress; Location destination, source; };
struct SavedTab {
    Location path;
    fs::path focus, anchor;
    Options options;
    std::vector<Location> history, expanded;
    size_t historyIndex = 0;
};
struct SavedPanel { std::vector<SavedTab> tabs; size_t active = 0; bool treeVisible = false; int treeWidth = 170; };
struct SavedState { std::vector<Location> bookmarks; std::array<SavedPanel,2> panels; int activePanel = 0, splitPercent = 50, width = 1200, height = 760; };

class Core {
public:
    explicit Core(std::shared_ptr<Provider> = filesystem_provider());
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    const Session& state() const { return state_; }
    const Tab* tab(int panel, Id id = 0) const;
    const Job* job(Id) const;
    Result execute(const Command&);
    bool poll(); // Call on the owning session thread; workers only publish results.
    void start(const Location& left, const Location& right);
    void restore(const SavedState&);
    SavedState save() const;
    bool busy() const;
    bool searching() const; // separate from busy(): searches may run for a long time
    std::vector<fs::path> take_viewers() { auto out = std::move(viewers_); viewers_.clear(); return out; }
private:
    struct Work {
        enum Kind { Scan, View, Copy } kind = Scan;
        JobKind op = JobKind::Copy; // transfer-worker operation when kind == Copy
        std::string name; bool trash = false;
        std::vector<Location> sources;
        Location destination;
        std::shared_ptr<std::atomic<uint64_t>> bytes;
        std::shared_ptr<TransferProgress> progress;
        size_t queuedBytes = 0;
        int panel = 0; Id tab = 0, node = 0, generation = 0, job = 0;
        Location path; fs::path focusAfter;
        int historyTarget = -1;
        bool refresh = false;
        Options options;
        Cancel cancel;
        size_t budget = 0;
    };
    struct Done { Work work; Listing listing; fs::path viewer; };
    std::vector<fs::path> viewers_;
    std::shared_ptr<Provider> provider_;
    Session state_;
    std::vector<Location> roots_;
    std::map<Id, Job> jobs_;
    Id next_ = 1;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Work> queue_;
    std::deque<Work> copies_;
    size_t copyBytes_ = 0;
    std::deque<Done> done_;
    std::vector<std::thread> workers_;
    std::vector<Cancel> running_;
    bool stopping_ = false;
    Tab* mutable_tab(int, Id = 0);
    void worker(bool copies = false);
    bool submit(Work);
    Result navigate(int, Tab&, Location, int historyTarget = -1, bool refresh = false, fs::path focusAfter = {});
    void init_tree(Tab&);
    Id reveal(Tab&, const Location&);
    Result expand(int, Tab&, Id);
    void cancel(Tab&);
    void prune_jobs();
    void stop_search(Tab&);
    bool publish_search(Tab&, bool force);
    Result queue_operation(const Command&, Tab&);
};
}
