#pragma once
#include "core/core.h"
#include <functional>
#include <optional>

namespace ac {
// Read-only archive browsing. An archive behaves like a folder: F:\x\a.zip\dir\file.txt and
// ssh://h/x/a.tar.gz/dir are ordinary Locations; the first path component with an archive
// extension that is a regular file is the container. Nested archives are not opened.
struct ArchivePath {
    Location container;   // the archive file
    std::string inner;    // member path inside, '/'-separated, no leading slash; empty = archive root
};
bool archive_name(const fs::path& name);
// Stat callback: returns true and sets regular/size/mtime when the location exists.
struct ArchiveStat { bool exists = false, regular = false; uint64_t size = 0; int64_t modified = 0; };
using ArchiveStatFn = std::function<ArchiveStat(const Location&)>;
// Purely syntactic unless a component looks like an archive; then one stat confirms it is a file.
std::optional<ArchivePath> locate_archive(const Location&, const ArchiveStatFn&);
Listing archive_list(const ArchivePath&, const ArchiveStat& container, bool directoriesOnly, const Cancel&, size_t budget);
// Member metadata (directories included). Throws when the member does not exist.
Entry archive_entry(const ArchivePath&, const ArchiveStat& container, const Cancel&);
// Streams a regular member's bytes to sink; verifies size and CRC where the format has one.
void archive_extract(const ArchivePath&, const ArchiveStat& container, const Cancel&, const std::function<void(const char*,size_t)>& sink);

// Exposed for tests.
std::string gunzip(const std::string& data);
std::string inflate_raw(const std::string& data);
uint32_t crc32(uint32_t crc,const void* data,size_t size);
}
