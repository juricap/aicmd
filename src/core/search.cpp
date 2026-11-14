#include "search.h"
#include "fs/ssh.h"
#include <chrono>

namespace ac {
using Clock = std::chrono::steady_clock;
static const char* fastLane(int i) { static const char* names[] = {"search-1","search-2","search-3","search-4"}; return names[i & 3]; }
static fs::path join(const Location& root,const fs::path& relative,const fs::path& name) {
    if (relative.empty()) return name;
    // Remote relative paths keep POSIX separators so Location::operator/ appends them verbatim.
    return root.is_remote() ? fs::u8path(relative.u8string()+"/"+name.u8string()) : relative/name;
}
Search::Search(std::shared_ptr<Provider> provider, SearchQuery query, SearchTuning tuning)
    : provider_(std::move(provider)), query_(std::move(query)), tuning_(tuning) {
    for (auto& c : query_.text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c-'A'+'a');
    if (query_.mask.empty()) query_.mask = "*";
    tuning_.fastLanes = std::clamp(tuning_.fastLanes,1,4);
    fast_[0].push_back({query_.root,{},0,order_++}); fastCount_ = 1;
    for (int i = 0; i < tuning_.fastLanes; ++i) threads_.emplace_back([this,i]{ fast(i); });
    threads_.emplace_back([this]{ slow(); });
    if (!query_.text.empty()) threads_.emplace_back([this]{ content(); });
}
Search::~Search() { cancel(); for (auto& t : threads_) t.join(); }
void Search::cancel() { cancel_->store(true); std::lock_guard<std::mutex> lock(mutex_); cv_.notify_all(); }
std::vector<SearchHit> Search::take() { std::lock_guard<std::mutex> lock(mutex_); std::vector<SearchHit> out; out.swap(hits_); return out; }
SearchStats Search::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = stats_; s.queued = fastCount_; s.slowQueued = slow_.size(); s.contentQueued = content_.size();
    if (cancel_->load() && busy_ == 0) s.finished = true;
    return s;
}
void Search::consider(const Folder& folder, Entry&& e, Batch& batch) {
    ++batch.entries;
    if (!query_.hidden && (e.hidden || e.system)) return;
    auto relative = join(query_.root,folder.relative,e.name);
    if ((!e.directory || query_.directories) && matches_pattern(e.name,query_.mask)) {
        if (query_.text.empty()) batch.hits.push_back({relative,e});
        else if (!e.directory && !e.link) batch.content.push_back({relative,e});
    }
    if (e.directory && !e.link && folder.depth < 256) batch.folders.push_back({folder.path/e.name,relative,folder.depth+1,0});
}
void Search::commit(Batch& batch, std::unique_lock<std::mutex>& lock) {
    stats_.entries += batch.entries; batch.entries = 0;
    for (auto& f : batch.folders) { f.order = order_++; fast_[f.depth].push_back(std::move(f)); ++fastCount_; }
    for (auto& h : batch.hits) {
        if (stats_.hits >= query_.maxResults) { stats_.truncated = true; cancel_->store(true); break; }
        ++stats_.hits; h.entry.id = stats_.hits; hits_.push_back(std::move(h));
    }
    // Bound memory: the content lane is the slowest consumer, so listers wait for it.
    if (!batch.content.empty()) cv_.wait(lock,[&]{ return cancel_->load() || content_.size() < 100000; });
    for (auto& c : batch.content) content_.push_back(std::move(c));
    batch.folders.clear(); batch.hits.clear(); batch.content.clear();
    cv_.notify_all();
}
void Search::finish_if_idle() {
    if (!fastCount_ && slow_.empty() && content_.empty() && !busy_) { stats_.finished = true; cv_.notify_all(); }
}
void Search::fast(int lane) {
    for (;;) {
        Folder folder;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{ return cancel_->load() || stats_.finished || fastCount_; });
            if (cancel_->load() || stats_.finished) return;
            // Breadth-first keeps shallow results first; when the frontier grows too large, drain deepest-first.
            auto it = fastCount_ > tuning_.maxQueued ? std::prev(fast_.end()) : fast_.begin();
            folder = std::move(it->second.front()); it->second.pop_front(); if (it->second.empty()) fast_.erase(it);
            --fastCount_; ++busy_;
        }
        // Probe: a capped listing. Overflow means "too big for the fast lane" and is discarded, not processed twice.
        std::vector<Entry> entries; bool overflow = false; auto start = Clock::now();
        auto error = provider_->enumerate(folder.path,cancel_,[&](Entry&& e) {
            entries.push_back(std::move(e));
            if (entries.size() > tuning_.probeEntries || ((entries.size() & 63) == 0 && Clock::now()-start > std::chrono::milliseconds(tuning_.probeMillis))) { overflow = true; return false; }
            return true;
        },fastLane(lane));
        Batch batch;
        if (!overflow && !error) for (auto& e : entries) consider(folder,std::move(e),batch);
        std::unique_lock<std::mutex> lock(mutex_);
        if (overflow) { ++stats_.slowFolders; slow_.push_back(std::move(folder)); }
        else if (error) { if (error.code != "cancelled") { ++stats_.unreadable; stats_.lastError = display_path(folder.path)+": "+error.message; } }
        else { ++stats_.folders; commit(batch,lock); }
        --busy_; finish_if_idle(); cv_.notify_all();
    }
}
void Search::slow() {
    for (;;) {
        Folder folder;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{ return cancel_->load() || stats_.finished || !slow_.empty(); });
            if (cancel_->load() || stats_.finished) return;
            folder = std::move(slow_.front()); slow_.pop_front(); ++busy_;
        }
        // Stream the large folder, publishing matches and subfolders as it goes.
        Batch batch; auto flushed = Clock::now();
        auto error = provider_->enumerate(folder.path,cancel_,[&](Entry&& e) {
            consider(folder,std::move(e),batch);
            if (batch.entries >= 2048 || Clock::now()-flushed > std::chrono::milliseconds(100)) {
                std::unique_lock<std::mutex> lock(mutex_); commit(batch,lock); flushed = Clock::now();
            }
            return !cancel_->load();
        },"search-slow");
        std::unique_lock<std::mutex> lock(mutex_);
        commit(batch,lock);
        if (error && error.code != "cancelled") { ++stats_.unreadable; stats_.lastError = display_path(folder.path)+": "+error.message; }
        else ++stats_.folders;
        --busy_; finish_if_idle(); cv_.notify_all();
    }
}
void Search::content() {
    for (;;) {
        SearchHit candidate;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock,[&]{ return cancel_->load() || stats_.finished || !content_.empty(); });
            if (cancel_->load() || stats_.finished) return;
            candidate = std::move(content_.front()); content_.pop_front(); ++busy_;
            cv_.notify_all();
        }
        bool found = false; std::string failure;
        try { found = file_contains(query_.root/candidate.relative,query_.text,cancel_); }
        catch (const std::exception& e) { failure = e.what(); }
        std::unique_lock<std::mutex> lock(mutex_);
        ++stats_.contentChecked;
        if (!failure.empty() && !cancel_->load()) { ++stats_.unreadable; stats_.lastError = display_path(candidate.relative)+": "+failure; }
        if (found) { Batch batch; batch.hits.push_back(std::move(candidate)); commit(batch,lock); }
        --busy_; finish_if_idle(); cv_.notify_all();
    }
}
}
