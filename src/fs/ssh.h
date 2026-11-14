#pragma once
#include "core/core.h"
#include <functional>
namespace ac {
fs::path ssh_config_path();
std::vector<std::string> ssh_hosts(const fs::path& config = ssh_config_path());
std::shared_ptr<Provider> filesystem_provider();
using Progress = std::function<void(uint64_t)>;
// Regular-file copy. Exclusive destination creation; never silently overwrites.
void copy_file(const Location& source,const Location& destination,const Cancel&,const Progress& = {});
void copy_location(const Location& source,const Location& destination,const Cancel&,const Progress& = {});
// Throws unless name is one component valid on the parent's filesystem.
void validate_name(const Location& parent,const fs::path& name);
void make_directory(const Location&,const Cancel&);
// Same-host no-overwrite rename. Returns false when a copy is required (another host or volume).
bool rename_location(const Location& from,const Location& to,const Cancel&);
// Removes links, never their targets. trash=true uses the Windows Recycle Bin; unsupported elsewhere.
void delete_location(const Location&,bool trash,const Cancel&,const std::function<void()>& removed = {});
// Rename when possible; otherwise copy, then delete the source only after the copy completes.
void move_location(const Location& source,const Location& destination,const Cancel&,const Progress& = {});
// Streams a regular file looking for needle (already ASCII-lowercased), ASCII case-insensitively.
bool file_contains(const Location&,const std::string& needle,const Cancel&,const char* lane = "search-content");
fs::path stage_view(const Location&,const Cancel&,const Progress& = {});
}
