#pragma once
#include "core/core.h"
namespace ac {
// A source is owned by a single demux worker. Cancellation may come from any thread.
class ReadSource {
public:
    virtual ~ReadSource() = default;
    virtual uint64_t size() const = 0;
    virtual std::string read_at(uint64_t offset,size_t length) = 0;
};
std::unique_ptr<ReadSource> open_read(const Location&,const Cancel&);
}
