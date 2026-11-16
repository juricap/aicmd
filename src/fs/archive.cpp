#include "archive.h"
#include "read_source.h"
#include "sevenzip.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <list>
#include <mutex>

namespace ac {
namespace {
// ---------------------------------------------------------------- CRC-32
const std::array<uint32_t,256>& crc_table() {
    static const auto table = []{ std::array<uint32_t,256> t{}; for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } return t; }();
    return table;
}
struct Stop {}; // thrown by sinks to end decompression early

// ---------------------------------------------------------------- byte input with push-back
class Bytes {
public:
    using Pull = std::function<size_t(uint8_t*,size_t)>;
    explicit Bytes(Pull pull) : pull_(std::move(pull)), buffer_(256*1024+16) {}
    bool fill() {
        // Keep the last 16 bytes so up to 16 bytes can be returned after a bit reader over-reads.
        size_t keep = std::min<size_t>(16,end_);
        std::memmove(buffer_.data(),buffer_.data()+end_-keep,keep);
        pos_ = keep - (end_ - pos_); end_ = keep;
        size_t n = pull_(buffer_.data()+end_,buffer_.size()-end_);
        end_ += n; return n > 0;
    }
    int get() { if (pos_ == end_ && !fill()) return -1; return buffer_[pos_++]; }
    uint8_t need() { int c = get(); if (c < 0) throw std::runtime_error("Archive data is truncated"); return static_cast<uint8_t>(c); }
    void unget(size_t n) { if (n > pos_) throw std::runtime_error("Archive reader push-back overflow"); pos_ -= n; }
    void skip(uint64_t n) { while (n) { if (pos_ == end_ && !fill()) throw std::runtime_error("Archive data is truncated"); auto k = std::min<uint64_t>(n,end_-pos_); pos_ += static_cast<size_t>(k); n -= k; } }
    bool at_end() { return pos_ == end_ && !fill(); }
private:
    Pull pull_;
    std::vector<uint8_t> buffer_;
    size_t pos_ = 0, end_ = 0;
};

// ---------------------------------------------------------------- inflate (RFC 1951)
constexpr int FAST = 10;
struct Huffman {
    uint16_t count[16]{}; uint16_t symbol[320]{}; uint16_t fast[1 << FAST]{};
    void build(const uint8_t* lengths,int n) {
        std::memset(count,0,sizeof(count)); std::memset(fast,0,sizeof(fast));
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1; for (int len = 1; len < 16; ++len) { left <<= 1; left -= count[len]; if (left < 0) throw std::runtime_error("Invalid deflate code lengths"); }
        uint16_t offs[17]{}; for (int len = 1; len < 16; ++len) offs[len+1] = offs[len]+count[len];
        for (int s = 0; s < n; ++s) if (lengths[s]) symbol[offs[lengths[s]]++] = static_cast<uint16_t>(s);
        // Fast table: deflate stores codes MSB-first in an LSB-first bit stream, so index by reversed code.
        unsigned code = 0, index = 0;
        for (int len = 1; len <= FAST; ++len) {
            for (unsigned k = 0; k < count[len]; ++k, ++code, ++index) {
                unsigned rev = 0; for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1u) << (len-1-b);
                for (unsigned j = rev; j < (1u << FAST); j += 1u << len) fast[j] = static_cast<uint16_t>((len << 9) | symbol[index]);
            }
            code <<= 1;
        }
    }
};
class Inflater {
public:
    Inflater(Bytes& in,const std::function<void(const uint8_t*,size_t)>& sink) : in_(in), sink_(sink), buf_out_(BUFFER) {}
    uint64_t run() {
        for (bool last = false; !last;) {
            last = bits(1); int type = static_cast<int>(bits(2));
            if (type == 0) stored();
            else if (type == 1) { fixed(); codes(fixedLit_,fixedDist_); }
            else if (type == 2) { dynamic(); codes(lit_,dist_); }
            else throw std::runtime_error("Invalid deflate block type");
        }
        flush();
        size_t unread = static_cast<size_t>(count_ / 8); in_.unget(unread > phantom_ ? unread - phantom_ : 0); count_ = 0; buf_ = 0; // return whole unread input bytes
        return total_;
    }
private:
    Bytes& in_;
    const std::function<void(const uint8_t*,size_t)>& sink_;
    uint64_t buf_ = 0; int count_ = 0; size_t phantom_ = 0;
    // One flat buffer is both history and pending output: [0,pos_) holds data, [flushed_,pos_) is unsent.
    // When nearly full, unsent bytes are flushed and the last 32 KiB of history moves to the front.
    static constexpr size_t BUFFER = 1 << 20, HISTORY = 32768;
    std::vector<uint8_t> buf_out_; size_t pos_ = 0, flushed_ = 0; uint64_t total_ = 0;
    Huffman lit_, dist_, fixedLit_, fixedDist_; bool fixedReady_ = false;
    void refill(int want) {
        while (count_ < want) {
            int c = in_.get();
            if (c < 0) { ++phantom_; c = 0; } // zero padding allows peeking; consuming it is an error
            buf_ |= static_cast<uint64_t>(c) << count_; count_ += 8;
        }
    }
    void consume(int n) {
        buf_ >>= n; count_ -= n;
        if (phantom_ && static_cast<size_t>(count_) < phantom_*8) throw std::runtime_error("Deflate stream is truncated");
    }
    uint32_t bits(int n) { if (!n) return 0; refill(n); uint32_t v = static_cast<uint32_t>(buf_ & ((1ull << n)-1)); consume(n); return v; }
    int decode(const Huffman& h) {
        refill(15);
        auto e = h.fast[buf_ & ((1u << FAST)-1)];
        if (e) { consume(e >> 9); return e & 511; }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>((buf_ >> (len-1)) & 1);
            int c = h.count[len];
            if (code - c < first) { consume(len); return h.symbol[index + (code-first)]; }
            index += c; first += c; first <<= 1; code <<= 1;
        }
        throw std::runtime_error("Invalid deflate code");
    }
    void room() {
        if (pos_ + 258 <= BUFFER) return;
        flush();
        std::memmove(buf_out_.data(),buf_out_.data()+pos_-HISTORY,HISTORY);
        pos_ = flushed_ = HISTORY;
    }
    void put(uint8_t b) { room(); buf_out_[pos_++] = b; }
    void flush() { if (pos_ > flushed_) { sink_(buf_out_.data()+flushed_,pos_-flushed_); total_ += pos_-flushed_; flushed_ = pos_; } }
    uint64_t produced() const { return total_ + (pos_ - flushed_); }
    void stored() {
        consume(count_ % 8);
        uint32_t len = bits(16), nlen = bits(16);
        if ((len ^ 0xFFFF) != nlen) throw std::runtime_error("Invalid stored deflate block");
        while (len--) put(static_cast<uint8_t>(bits(8)));
    }
    void fixed() {
        if (fixedReady_) return;
        uint8_t l[320]; int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        fixedLit_.build(l,288);
        for (i = 0; i < 30; ++i) l[i] = 5;
        fixedDist_.build(l,30); fixedReady_ = true;
    }
    void dynamic() {
        int nlen = static_cast<int>(bits(5))+257, ndist = static_cast<int>(bits(5))+1, ncode = static_cast<int>(bits(4))+4;
        if (nlen > 286 || ndist > 30) throw std::runtime_error("Invalid deflate header");
        static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        uint8_t lengths[320]{};
        for (int i = 0; i < ncode; ++i) lengths[order[i]] = static_cast<uint8_t>(bits(3));
        Huffman lencode; lencode.build(lengths,19);
        uint8_t all[320]{};
        for (int i = 0; i < nlen+ndist;) {
            int s = decode(lencode);
            if (s < 16) { all[i++] = static_cast<uint8_t>(s); continue; }
            int repeat = 0; uint8_t value = 0;
            if (s == 16) { if (!i) throw std::runtime_error("Invalid deflate repeat"); value = all[i-1]; repeat = 3+static_cast<int>(bits(2)); }
            else if (s == 17) repeat = 3+static_cast<int>(bits(3));
            else repeat = 11+static_cast<int>(bits(7));
            if (i+repeat > nlen+ndist) throw std::runtime_error("Invalid deflate repeat length");
            while (repeat--) all[i++] = value;
        }
        if (!all[256]) throw std::runtime_error("Deflate block has no end code");
        lit_.build(all,nlen); dist_.build(all+nlen,ndist);
    }
    void codes(const Huffman& lit,const Huffman& dist) {
        static const uint16_t lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static const uint8_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static const uint16_t dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        static const uint8_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
        for (;;) {
            int s = decode(lit);
            if (s < 256) { put(static_cast<uint8_t>(s)); continue; }
            if (s == 256) return;
            s -= 257; if (s >= 29) throw std::runtime_error("Invalid deflate length code");
            uint32_t len = lbase[s] + bits(lext[s]);
            int d = decode(dist); if (d >= 30) throw std::runtime_error("Invalid deflate distance code");
            uint32_t distance = dbase[d] + bits(dext[d]);
            if (distance > produced() || distance > 32768) throw std::runtime_error("Deflate distance beyond output");
            room();
            uint8_t* to = buf_out_.data()+pos_; const uint8_t* from = to - distance;
            if (distance >= len) std::memcpy(to,from,len);
            else for (uint32_t i = 0; i < len; ++i) to[i] = from[i]; // overlapping run
            pos_ += len;
        }
    }
};
// gzip (RFC 1952), including concatenated members.
void gunzip_stream(Bytes& in,const std::function<void(const uint8_t*,size_t)>& sink) {
    bool first = true;
    while (first || !in.at_end()) {
        first = false;
        if (in.need() != 0x1f || in.need() != 0x8b || in.need() != 8) throw std::runtime_error("Not a gzip stream");
        int flags = in.need(); in.skip(6);
        if (flags & 4) { int n = in.need(); n |= in.need() << 8; in.skip(n); }
        if (flags & 8) while (in.need()) {}
        if (flags & 16) while (in.need()) {}
        if (flags & 2) in.skip(2);
        uint32_t crc = 0xFFFFFFFFu;
        std::function<void(const uint8_t*,size_t)> checked = [&](const uint8_t* p,size_t n) {
            const auto& t = crc_table(); for (size_t i = 0; i < n; ++i) crc = t[(crc ^ p[i]) & 255] ^ (crc >> 8);
            sink(p,n);
        };
        Inflater inflater(in,checked); uint64_t size = inflater.run();
        uint32_t stored = 0, isize = 0;
        for (int i = 0; i < 4; ++i) stored |= static_cast<uint32_t>(in.need()) << (8*i);
        for (int i = 0; i < 4; ++i) isize |= static_cast<uint32_t>(in.need()) << (8*i);
        if ((crc ^ 0xFFFFFFFFu) != stored || static_cast<uint32_t>(size) != isize) throw std::runtime_error("gzip checksum mismatch");
    }
}

// ---------------------------------------------------------------- archive index
enum class Kind { Zip, Tar, TarGz, Seven /* 7z.dll archive */, TarSeven /* TAR inside an xz/bzip2 stream via 7z.dll */ };
struct Member {
    std::string path; Entry entry;
    uint64_t offset = 0, packed = 0; int method = 0; uint32_t crc = 0;
    bool hasCrc = false, encrypted = false, synthetic = false;
};
struct Index {
    Kind kind = Kind::Zip;
    std::vector<Member> members;
    std::map<std::string,size_t> byPath;
    std::map<std::string,std::vector<size_t>> children;
};
std::string read_exact(ReadSource& source,uint64_t offset,size_t length) {
    std::string out;
    while (out.size() < length) {
        auto part = source.read_at(offset+out.size(),length-out.size());
        if (part.empty()) throw std::runtime_error("Archive is truncated");
        out += part;
    }
    return out;
}
uint32_t le16(const std::string& s,size_t at) { if (at+2 > s.size()) throw std::runtime_error("Archive header is truncated"); return static_cast<uint8_t>(s[at]) | static_cast<uint8_t>(s[at+1]) << 8; }
uint32_t le32(const std::string& s,size_t at) { return le16(s,at) | le16(s,at+2) << 16; }
uint64_t le64(const std::string& s,size_t at) { return le32(s,at) | static_cast<uint64_t>(le32(s,at+4)) << 32; }
std::string cp437(const std::string& s) {
    static const char16_t high[128] = {
        0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xA2,0xA3,0xA5,0x20A7,0x192,
        0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,0x2310,0xAC,0xBD,0xBC,0xA1,0xAB,0xBB,0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
        0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
        0x3B1,0xDF,0x393,0x3C0,0x3A3,0x3C3,0xB5,0x3C4,0x3A6,0x398,0x3A9,0x3B4,0x221E,0x3C6,0x3B5,0x2229,0x2261,0xB1,0x2265,0x2264,0x2320,0x2321,0xF7,0x2248,0xB0,0x2219,0xB7,0x221A,0x207F,0xB2,0x25A0,0xA0};
    std::string out;
    for (unsigned char c : s) {
        unsigned u = c < 128 ? c : high[c-128];
        if (u < 0x80) out += static_cast<char>(u);
        else if (u < 0x800) { out += static_cast<char>(0xC0 | (u >> 6)); out += static_cast<char>(0x80 | (u & 63)); }
        else { out += static_cast<char>(0xE0 | (u >> 12)); out += static_cast<char>(0x80 | ((u >> 6) & 63)); out += static_cast<char>(0x80 | (u & 63)); }
    }
    return out;
}
// Normalises a stored name; returns empty for names that could escape (.., absolute, drive) or are empty.
std::string clean(std::string name,bool& directory) {
    std::replace(name.begin(),name.end(),'\\','/');
    directory = !name.empty() && name.back() == '/';
    std::string out; size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('/',start); if (end == std::string::npos) end = name.size();
        auto part = name.substr(start,end-start);
        if (part == "..") return {};
        if (part.find('\0') != std::string::npos) return {};
        if (!part.empty() && part != "." && !(out.empty() && part.size() == 2 && part[1] == ':')) { if (!out.empty()) out += '/'; out += part; }
        start = end+1;
    }
    return out;
}
int64_t dos_time(uint32_t time,uint32_t date) {
    // DOS times are local wall-clock; treated as UTC (an approximation; extended timestamps override it).
    int y = 1980 + static_cast<int>(date >> 9), m = static_cast<int>((date >> 5) & 15), d = static_cast<int>(date & 31);
    if (m < 1 || m > 12 || d < 1) return 0;
    y -= m <= 2; int era = y / 400; int yoe = y - era*400; int doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1; int doe = yoe*365 + yoe/4 - yoe/100 + doy;
    int64_t days = static_cast<int64_t>(era)*146097 + doe - 719468;
    return days*86400 + (time >> 11)*3600 + ((time >> 5) & 63)*60 + (time & 31)*2;
}
void add(Index& index,Member m) {
    if (m.path.empty()) return;
    auto found = index.byPath.find(m.path);
    if (found != index.byPath.end()) { if (index.members[found->second].synthetic || !m.entry.directory) index.members[found->second] = std::move(m); return; }
    if (index.members.size() >= 2000000) throw std::runtime_error("Archive has too many entries");
    index.byPath[m.path] = index.members.size(); index.members.push_back(std::move(m));
}
void finish(Index& index) {
    // Synthesize missing parent folders, then build child lists.
    for (size_t i = 0; i < index.members.size(); ++i) {
        auto path = index.members[i].path;
        for (auto slash = path.rfind('/'); slash != std::string::npos; slash = path.rfind('/')) {
            path.resize(slash);
            if (index.byPath.count(path)) break;
            Member dir; dir.path = path; dir.synthetic = true; dir.entry.directory = true; dir.entry.name = fs::u8path(path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/')+1));
            add(index,std::move(dir));
        }
    }
    for (size_t i = 0; i < index.members.size(); ++i) {
        const auto& p = index.members[i].path; auto slash = p.rfind('/');
        index.children[slash == std::string::npos ? std::string() : p.substr(0,slash)].push_back(i);
    }
}
Index read_zip(ReadSource& source,const Cancel& cancel) {
    uint64_t size = source.size();
    if (size < 22) throw std::runtime_error("Not a ZIP archive");
    size_t tailSize = static_cast<size_t>(std::min<uint64_t>(size,65557));
    auto tail = read_exact(source,size-tailSize,tailSize);
    size_t eocd = std::string::npos;
    for (size_t i = tailSize-22 + 1; i-- > 0;) if (le32(tail,i) == 0x06054b50u) { eocd = i; break; }
    if (eocd == std::string::npos) throw std::runtime_error("Not a ZIP archive (no end of central directory)");
    uint64_t entries = le16(tail,eocd+10), cdSize = le32(tail,eocd+12), cdOffset = le32(tail,eocd+16);
    if ((entries == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu) && eocd >= 20 && le32(tail,eocd-20) == 0x07064b50u) {
        auto at = le64(tail,eocd-12);
        auto z = read_exact(source,at,56);
        if (le32(z,0) != 0x06064b50u) throw std::runtime_error("Invalid ZIP64 end record");
        entries = le64(z,32); cdSize = le64(z,40); cdOffset = le64(z,48);
    }
    if (cdOffset > size || cdSize > size-cdOffset || cdSize > 512ull*1024*1024) throw std::runtime_error("Invalid ZIP central directory");
    auto cd = read_exact(source,cdOffset,static_cast<size_t>(cdSize));
    Index index; index.kind = Kind::Zip;
    size_t at = 0;
    for (uint64_t n = 0; n < entries && at+46 <= cd.size(); ++n) {
        if ((n & 1023) == 0 && cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (le32(cd,at) != 0x02014b50u) throw std::runtime_error("Invalid ZIP central directory entry");
        uint32_t made = le16(cd,at+4), flags = le16(cd,at+8), method = le16(cd,at+10), time = le16(cd,at+12), date = le16(cd,at+14);
        uint32_t crc = le32(cd,at+16); uint64_t packed = le32(cd,at+20), unpacked = le32(cd,at+24);
        size_t nameLen = le16(cd,at+28), extraLen = le16(cd,at+30), commentLen = le16(cd,at+32);
        uint32_t external = le32(cd,at+38); uint64_t offset = le32(cd,at+42);
        if (at+46+nameLen+extraLen+commentLen > cd.size()) throw std::runtime_error("Truncated ZIP central directory");
        auto rawName = cd.substr(at+46,nameLen);
        auto extra = cd.substr(at+46+nameLen,extraLen);
        int64_t modified = dos_time(time,date);
        for (size_t e = 0; e+4 <= extra.size();) {
            uint32_t id = le16(extra,e), len = le16(extra,e+2); size_t body = e+4;
            if (body+len > extra.size()) break;
            if (id == 0x0001) {
                size_t p = body;
                if (unpacked == 0xFFFFFFFFu && p+8 <= body+len) { unpacked = le64(extra,p); p += 8; }
                if (packed == 0xFFFFFFFFu && p+8 <= body+len) { packed = le64(extra,p); p += 8; }
                if (offset == 0xFFFFFFFFu && p+8 <= body+len) { offset = le64(extra,p); p += 8; }
            } else if (id == 0x5455 && len >= 5 && (extra[body] & 1)) modified = static_cast<int32_t>(le32(extra,body+1));
            e = body+len;
        }
        at += 46+nameLen+extraLen+commentLen;
        bool directory = false;
        auto path = clean(flags & 0x800 ? rawName : cp437(rawName),directory);
        uint32_t unixMode = (made >> 8) == 3 ? external >> 16 : 0;
        if ((unixMode & 0170000) == 0040000 || (external & 0x10)) directory = true;
        Member m; m.path = path; m.method = static_cast<int>(method); m.crc = crc; m.hasCrc = true; m.encrypted = (flags & 1) != 0;
        m.offset = offset; m.packed = packed;
        m.entry.name = fs::u8path(path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/')+1));
        m.entry.directory = directory; m.entry.link = (unixMode & 0170000) == 0120000; m.entry.modified = modified;
        if (!directory) m.entry.size = unpacked;
        add(index,std::move(m));
    }
    finish(index); return index;
}
// TAR (ustar, GNU long names, pax path/size). Headers are parsed the same way for plain and gzip streams.
uint64_t octal(const char* p,size_t n) {
    if (static_cast<unsigned char>(p[0]) & 0x80) { uint64_t v = static_cast<unsigned char>(p[0]) & 0x7F; for (size_t i = 1; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(p[i]); return v; }
    uint64_t v = 0; size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v*8 + static_cast<uint64_t>(p[i]-'0');
    return v;
}
struct TarParser {
    explicit TarParser(Index& target) : index(target) {}
    Index& index; std::string longName, paxPath; std::optional<uint64_t> paxSize; int zeros = 0; bool ended = false;
    // Returns the data size following this header and whether that data must be passed to meta().
    std::pair<uint64_t,bool> header(const char* h,uint64_t dataOffset) {
        bool zero = std::all_of(h,h+512,[](char c){ return c == 0; });
        if (zero) { if (++zeros >= 2) ended = true; return {0,false}; }
        zeros = 0;
        unsigned sum = 0; for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(h[i]);
        if (sum != octal(h+148,8)) throw std::runtime_error("Invalid TAR header checksum");
        uint64_t size = octal(h+124,12); char type = h[156];
        if (type == 'L' || type == 'x') { pending = type; return {size,true}; }
        if (type == 'g' || type == 'K' || type == 'V') return {size,false};
        std::string name;
        if (!paxPath.empty()) name = paxPath;
        else if (!longName.empty()) name = longName;
        else {
            name.assign(h,strnlen(h,100));
            if (!std::memcmp(h+257,"ustar",5) && h[345]) name = std::string(h+345,strnlen(h+345,155)) + "/" + name;
        }
        if (paxSize) size = *paxSize;
        longName.clear(); paxPath.clear(); paxSize.reset();
        bool directory = type == '5';
        Member m; m.path = clean(name,directory); m.offset = dataOffset; m.packed = size;
        m.entry.name = fs::u8path(m.path.substr(m.path.rfind('/') == std::string::npos ? 0 : m.path.rfind('/')+1));
        m.entry.directory = directory; m.entry.link = type == '1' || type == '2';
        m.entry.modified = static_cast<int64_t>(octal(h+136,12));
        if (!directory) m.entry.size = m.entry.link ? 0 : size;
        add(index,std::move(m));
        return {(directory || type == '2' || type == '1') ? (type == '5' ? 0 : size) : size,false};
    }
    void meta(const std::string& data) {
        if (pending == 'L') longName = data.substr(0,strnlen(data.c_str(),data.size()));
        else for (size_t at = 0; at < data.size();) {
            auto space = data.find(' ',at); if (space == std::string::npos) break;
            size_t len = std::strtoul(data.c_str()+at,nullptr,10); if (!len || at+len > data.size()) break;
            auto record = data.substr(space+1,at+len-space-2); auto eq = record.find('=');
            if (eq != std::string::npos) {
                auto key = record.substr(0,eq), value = record.substr(eq+1);
                if (key == "path") paxPath = value;
                if (key == "size") paxSize = std::strtoull(value.c_str(),nullptr,10);
            }
            at += len;
        }
    }
    char pending = 0;
};
Index read_tar(ReadSource& source,const Cancel& cancel) {
    Index index; index.kind = Kind::Tar; TarParser parser(index);
    uint64_t at = 0, size = source.size();
    for (size_t n = 0; at+512 <= size && !parser.ended; ++n) {
        if ((n & 255) == 0 && cancel && cancel->load()) throw std::runtime_error("Cancelled");
        auto h = read_exact(source,at,512);
        auto [length,meta] = parser.header(h.data(),at+512);
        if (meta) { if (length > 16*1024*1024) throw std::runtime_error("TAR metadata record is too large"); parser.meta(read_exact(source,at+512,static_cast<size_t>(length))); }
        at += 512 + (length+511)/512*512;
    }
    finish(index); return index;
}
Bytes::Pull pull_from(ReadSource& source,uint64_t start,uint64_t length,const Cancel& cancel) {
    auto at = std::make_shared<uint64_t>(0);
    return [&source,start,length,cancel,at](uint8_t* out,size_t n) -> size_t {
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (*at >= length) return 0;
        auto part = source.read_at(start+*at,static_cast<size_t>(std::min<uint64_t>(n,length-*at)));
        std::memcpy(out,part.data(),part.size()); *at += part.size(); return part.size();
    };
}
// Push-based TAR walk over the decompressed gzip stream, used for indexing and extraction.
struct TarStream {
    explicit TarStream(Index& target) : parser(target) {}
    TarParser parser; std::string header, metaData; uint64_t position = 0, dataLeft = 0, padLeft = 0; bool collecting = false;
    std::function<void(uint64_t,const uint8_t*,size_t)> data; // (absolute offset, bytes)
    void feed(const uint8_t* p,size_t n) {
        while (n && !parser.ended) {
            if (dataLeft) {
                size_t k = static_cast<size_t>(std::min<uint64_t>(n,dataLeft));
                if (collecting) metaData.append(reinterpret_cast<const char*>(p),k); else if (data) data(position,p,k);
                p += k; n -= k; dataLeft -= k; position += k;
                if (!dataLeft && collecting) { parser.meta(metaData); metaData.clear(); collecting = false; }
                continue;
            }
            if (padLeft) { size_t k = static_cast<size_t>(std::min<uint64_t>(n,padLeft)); p += k; n -= k; padLeft -= k; position += k; continue; }
            size_t k = std::min(n,512-header.size()); header.append(reinterpret_cast<const char*>(p),k); p += k; n -= k; position += k;
            if (header.size() < 512) continue;
            auto [length,meta] = parser.header(header.data(),position); header.clear();
            if (meta && length > 16*1024*1024) throw std::runtime_error("TAR metadata record is too large");
            dataLeft = length; collecting = meta; padLeft = (512 - length % 512) % 512;
        }
        if (parser.ended) throw Stop{};
    }
};
Index read_targz(ReadSource& source,const Cancel& cancel) {
    Index index; index.kind = Kind::TarGz; TarStream stream(index);
    Bytes in(pull_from(source,0,source.size(),cancel));
    try { gunzip_stream(in,[&](const uint8_t* p,size_t n){ stream.feed(p,n); }); } catch (const Stop&) {}
    finish(index); return index;
}

// 7z, RAR, CAB, ISO... through 7z.dll; Member::offset holds the 7-Zip item index.
Index read_seven(ReadSource& source,const std::string& name,const Cancel& cancel) {
    Index index; index.kind = Kind::Seven;
    for (auto& item : sevenzip::list(source,name,cancel)) {
        bool slash = false;
        auto path = clean(item.path,slash);
        bool directory = item.directory || slash;
        Member m; m.path = path; m.offset = item.index; m.encrypted = item.encrypted;
        m.entry.name = fs::u8path(path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/')+1));
        m.entry.directory = directory; if (item.modified) m.entry.modified = item.modified;
        if (!directory) m.entry.size = item.size;
        add(index,std::move(m));
    }
    finish(index); return index;
}
// TAR.XZ / TAR.BZ2: 7z.dll decompresses the single stream (item 0), our TAR walker indexes it.
Index read_tar_seven(ReadSource& source,const std::string& name,const Cancel& cancel) {
    Index index; index.kind = Kind::TarSeven; TarStream stream(index);
    try { sevenzip::extract(source,name,0,cancel,[&](const uint8_t* p,size_t n){ stream.feed(p,n); }); } catch (const Stop&) {}
    finish(index); return index;
}

// ---------------------------------------------------------------- cache
struct Cached { std::string key; uint64_t size; int64_t modified; std::shared_ptr<const Index> index; };
std::mutex cacheMutex;
std::list<Cached> cache;
std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c-'A'+'a'); return s; }
bool ends(const std::string& s,const char* suffix) { auto n = std::strlen(suffix); return s.size() > n && !s.compare(s.size()-n,n,suffix); }
std::shared_ptr<const Index> load(const Location& container,const ArchiveStat& stat,const Cancel& cancel) {
    auto key = container.u8string();
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto it = cache.begin(); it != cache.end(); ++it) if (it->key == key) {
            if (it->size == stat.size && it->modified == stat.modified) { cache.splice(cache.begin(),cache,it); return cache.front().index; }
            cache.erase(it); break;
        }
    }
    auto source = open_read(container,cancel);
    auto header = source->size() >= 4 ? read_exact(*source,0,4) : std::string();
    auto name = lower(container.filename().u8string());
    std::shared_ptr<Index> index;
    auto seven = sevenzip::mode(name);
    if (seven == sevenzip::Mode::Archive) index = std::make_shared<Index>(read_seven(*source,name,cancel));
    else if (seven == sevenzip::Mode::TarStream) index = std::make_shared<Index>(read_tar_seven(*source,name,cancel));
    else if (header.size() == 4 && static_cast<uint8_t>(header[0]) == 0x1f && static_cast<uint8_t>(header[1]) == 0x8b) index = std::make_shared<Index>(read_targz(*source,cancel));
    else if (ends(name,".tar")) index = std::make_shared<Index>(read_tar(*source,cancel));
    else index = std::make_shared<Index>(read_zip(*source,cancel));
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.push_front({key,stat.size,stat.modified,index});
    while (cache.size() > 8) cache.pop_back();
    return index;
}
const Member& member(const Index& index,const std::string& inner) {
    auto it = index.byPath.find(inner);
    if (it == index.byPath.end()) throw std::runtime_error("Not found in archive: "+inner);
    return index.members[it->second];
}
}

uint32_t crc32(uint32_t crc,const void* data,size_t size) {
    const auto& t = crc_table(); auto p = static_cast<const uint8_t*>(data); crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = t[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
std::string inflate_raw(const std::string& data) {
    size_t at = 0; Bytes in([&](uint8_t* out,size_t n){ n = std::min(n,data.size()-at); std::memcpy(out,data.data()+at,n); at += n; return n; });
    std::string out; std::function<void(const uint8_t*,size_t)> sink = [&](const uint8_t* p,size_t n){ out.append(reinterpret_cast<const char*>(p),n); };
    Inflater(in,sink).run(); return out;
}
std::string gunzip(const std::string& data) {
    size_t at = 0; Bytes in([&](uint8_t* out,size_t n){ n = std::min(n,data.size()-at); std::memcpy(out,data.data()+at,n); at += n; return n; });
    std::string out; gunzip_stream(in,[&](const uint8_t* p,size_t n){ out.append(reinterpret_cast<const char*>(p),n); }); return out;
}
bool archive_name(const fs::path& name) {
    auto s = lower(name.u8string());
    for (auto suffix : {".zip",".jar",".war",".ear",".apk",".whl",".nupkg",".tar",".tgz",".tar.gz"}) if (ends(s,suffix)) return true;
    return sevenzip::mode(s) != sevenzip::Mode::None;
}
std::optional<ArchivePath> locate_archive(const Location& location,const ArchiveStatFn& stat) {
    std::vector<std::string> parts; Location prefix;
    if (location.is_remote()) {
        prefix = Location::ssh(location.host,"/");
        for (size_t at = 1; at < location.remote.size();) { auto end = location.remote.find('/',at); if (end == std::string::npos) end = location.remote.size(); if (end > at) parts.push_back(location.remote.substr(at,end-at)); at = end+1; }
    } else {
        prefix = location.local.root_path();
        for (const auto& part : location.local.relative_path()) if (!part.empty()) parts.push_back(part.u8string());
    }
    for (size_t i = 0; i < parts.size(); ++i) {
        prefix = prefix/fs::u8path(parts[i]);
        if (!archive_name(fs::u8path(parts[i]))) continue;
        auto s = stat(prefix);
        if (!s.exists) return std::nullopt;
        if (!s.regular) continue; // a folder named like an archive
        ArchivePath out; out.container = prefix;
        for (size_t j = i+1; j < parts.size(); ++j) { if (!out.inner.empty()) out.inner += '/'; out.inner += parts[j]; }
        return out;
    }
    return std::nullopt;
}
Listing archive_list(const ArchivePath& path,const ArchiveStat& container,bool dirs,const Cancel& cancel,size_t budget) {
    Listing out;
    try {
        auto index = load(path.container,container,cancel);
        if (!path.inner.empty()) { const auto& m = member(*index,path.inner); if (!m.entry.directory) return {{},{"invalid_path","Not a folder inside the archive"}}; }
        auto it = index->children.find(path.inner);
        if (it != index->children.end()) for (auto i : it->second) {
            const auto& e = index->members[i].entry;
            if (dirs && !e.directory) continue;
            out.bytes += sizeof(Entry)+e.name.native().size()*sizeof(fs::path::value_type)*2;
            if (out.bytes > budget) return {{},{"resource_limit","Archive folder exceeds the listing memory budget"}};
            out.entries.push_back(e); out.entries.back().id = out.entries.size();
        }
        out.readOnly = true;
    } catch (const std::exception& e) { return {{},{cancel && cancel->load() ? "cancelled" : "archive",e.what()}}; }
    return out;
}
Entry archive_entry(const ArchivePath& path,const ArchiveStat& container,const Cancel& cancel) {
    auto index = load(path.container,container,cancel);
    if (path.inner.empty()) { Entry e; e.directory = true; e.name = path.container.filename(); return e; }
    return member(*index,path.inner).entry;
}
void archive_extract(const ArchivePath& path,const ArchiveStat& container,const Cancel& cancel,const std::function<void(const char*,size_t)>& sink) {
    auto index = load(path.container,container,cancel);
    const auto& m = member(*index,path.inner);
    if (m.entry.directory || m.entry.link) throw std::runtime_error("Only regular archive members can be extracted");
    auto source = open_read(path.container,cancel);
    uint64_t size = m.entry.size.value_or(0), written = 0; uint32_t crc = 0;
    std::function<void(const uint8_t*,size_t)> checked = [&](const uint8_t* p,size_t n) {
        if (written+n > size) throw std::runtime_error("Archive member is larger than recorded");
        crc = crc32(crc,p,n); written += n; sink(reinterpret_cast<const char*>(p),n);
    };
    if (index->kind == Kind::Zip) {
        if (m.encrypted) throw std::runtime_error("Encrypted ZIP members are not supported");
        if (m.method != 0 && m.method != 8) throw std::runtime_error("Unsupported ZIP compression method "+std::to_string(m.method)+" (stored and deflate are supported)");
        auto local = read_exact(*source,m.offset,30);
        if (le32(local,0) != 0x04034b50u) throw std::runtime_error("Invalid ZIP local header");
        uint64_t data = m.offset+30+le16(local,26)+le16(local,28);
        if (data > source->size() || m.packed > source->size()-data) throw std::runtime_error("ZIP member exceeds archive");
        if (m.method == 0) {
            if (m.packed != size) throw std::runtime_error("Stored ZIP member size mismatch");
            for (uint64_t at = 0; at < size;) { if (cancel && cancel->load()) throw std::runtime_error("Cancelled"); auto part = source->read_at(data+at,static_cast<size_t>(std::min<uint64_t>(512*1024,size-at))); if (part.empty()) throw std::runtime_error("Archive is truncated"); checked(reinterpret_cast<const uint8_t*>(part.data()),part.size()); at += part.size(); }
        } else { Bytes in(pull_from(*source,data,m.packed,cancel)); Inflater(in,checked).run(); }
    } else if (index->kind == Kind::Seven) {
        if (m.encrypted) throw std::runtime_error("Encrypted archive members are not supported");
        sevenzip::extract(*source,lower(path.container.filename().u8string()),static_cast<uint32_t>(m.offset),cancel,checked);
    } else if (index->kind == Kind::TarSeven) {
        Index scratch; TarStream stream(scratch);
        uint64_t begin = m.offset, end = m.offset+size;
        stream.data = [&](uint64_t at,const uint8_t* p,size_t n) {
            uint64_t from = std::max(at,begin), to = std::min<uint64_t>(at+n,end);
            if (from < to) checked(p+(from-at),static_cast<size_t>(to-from));
            if (at+n >= end) throw Stop{};
        };
        if (size) { try { sevenzip::extract(*source,lower(path.container.filename().u8string()),0,cancel,[&](const uint8_t* p,size_t n){ stream.feed(p,n); }); } catch (const Stop&) {} }
    } else if (index->kind == Kind::Tar) {
        for (uint64_t at = 0; at < size;) { if (cancel && cancel->load()) throw std::runtime_error("Cancelled"); auto part = source->read_at(m.offset+at,static_cast<size_t>(std::min<uint64_t>(512*1024,size-at))); if (part.empty()) throw std::runtime_error("Archive is truncated"); checked(reinterpret_cast<const uint8_t*>(part.data()),part.size()); at += part.size(); }
    } else {
        // gzip has no random access: decompress from the start and forward the member's byte range.
        Index scratch; TarStream stream(scratch);
        uint64_t begin = m.offset, end = m.offset+size;
        stream.data = [&](uint64_t at,const uint8_t* p,size_t n) {
            uint64_t from = std::max(at,begin), to = std::min<uint64_t>(at+n,end);
            if (from < to) checked(p+(from-at),static_cast<size_t>(to-from));
            if (at+n >= end) throw Stop{};
        };
        Bytes in(pull_from(*source,0,source->size(),cancel));
        if (size) { try { gunzip_stream(in,[&](const uint8_t* p,size_t n){ stream.feed(p,n); }); } catch (const Stop&) {} }
    }
    if (written != size) throw std::runtime_error("Archive member is shorter than recorded");
    if (m.hasCrc && crc != m.crc) throw std::runtime_error("Archive member CRC mismatch");
}
}
