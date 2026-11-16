#pragma once
#include "read_source.h"
#include <functional>
#include <string>
#include <vector>

namespace ac::sevenzip {
// Formats read through 7-Zip's 7z.dll, loaded at runtime (Windows). Names are lower-case.
enum class Mode { None, Archive, TarStream };
// Archive: the file is a multi-member archive (7z, RAR, CAB, ISO...).
// TarStream: a single compressed stream (xz, bzip2) that holds a TAR.
Mode mode(const std::string& lowerName);
struct Item { std::string path; bool directory = false, encrypted = false; uint64_t size = 0; int64_t modified = 0; uint32_t index = 0; };
// Lists every item. Throws std::runtime_error (7z.dll missing, not an archive, encrypted headers...).
std::vector<Item> list(ReadSource&,const std::string& lowerName,const Cancel&);
// Streams one item to sink. sink may throw; the exception is rethrown after 7-Zip unwinds.
void extract(ReadSource&,const std::string& lowerName,uint32_t index,const Cancel&,const std::function<void(const uint8_t*,size_t)>& sink);
// Path of the loaded 7z.dll, or empty with a reason in `why`.
std::string library(std::string* why = nullptr);
}
