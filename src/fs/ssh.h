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
fs::path stage_view(const Location&,const Cancel&,const Progress& = {});
}
