#pragma once
#include "core.h"

namespace ac {
struct SearchQuery {
    Location root;
    std::string mask = "*";       // semicolon-separated wildcards, case-insensitive
    std::string text;             // optional content substring (ASCII case-insensitive)
    bool hidden = false;          // include hidden/system entries (and descend into them)
    bool directories = true;      // directories may match the mask
    size_t maxResults = 100000;
};
struct SearchTuning {
    int fastLanes = 2;            // probing listers; shallow folders first
    size_t probeEntries = 4096;   // a folder larger than this moves to the slow lane
    int probeMillis = 150;        // ... as does one that takes longer to list
    size_t maxQueued = 200000;    // past this, fast lanes drain deepest-first to bound memory
};
struct SearchHit { fs::path relative; Entry entry; };
struct SearchStats {
    uint64_t folders = 0, entries = 0, hits = 0, slowFolders = 0, unreadable = 0, contentChecked = 0;
    size_t queued = 0, slowQueued = 0, contentQueued = 0;
    bool finished = false, truncated = false;
    std::string lastError;
};
// Lanes:
//  - fast lanes take folders in breadth-first order (shallowest, then oldest) and list each with a
//    small entry/time cap. The start folder is therefore listed completely before any child, and no
//    single child can stall traversal. Subfolders found are queued, never entered immediately.
//  - a folder that exceeds the cap is handed, unprocessed, to the single slow lane, which streams it
//    in full and feeds its subfolders back to the fast lanes while it continues.
//  - name matches needing a content check go to a separate content lane.
// Links and junctions are reported but never followed.
class Search {
public:
    Search(std::shared_ptr<Provider>, SearchQuery, SearchTuning = {});
    ~Search();
    Search(const Search&) = delete;
    Search& operator=(const Search&) = delete;
    void cancel();
    std::vector<SearchHit> take();   // hits found since the previous call
    SearchStats stats() const;
    const SearchQuery& query() const { return query_; }
private:
    struct Folder { Location path; fs::path relative; int depth = 0; uint64_t order = 0; };
    void fast(int lane);
    void slow();
    void content();
    struct Batch { std::vector<SearchHit> hits, content; std::vector<Folder> folders; uint64_t entries = 0; };
    void consider(const Folder&, Entry&&, Batch&);
    void commit(Batch&, std::unique_lock<std::mutex>&);
    void finish_if_idle();
    std::shared_ptr<Provider> provider_;
    SearchQuery query_;
    SearchTuning tuning_;
    Cancel cancel_ = std::make_shared<std::atomic<bool>>(false);
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<int,std::deque<Folder>> fast_;    // by depth, FIFO within a depth
    size_t fastCount_ = 0;
    std::deque<Folder> slow_;
    std::deque<SearchHit> content_;
    std::vector<SearchHit> hits_;
    SearchStats stats_;
    uint64_t order_ = 0;
    int busy_ = 0;
    std::vector<std::thread> threads_;
};
}
