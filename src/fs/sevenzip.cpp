#include "sevenzip.h"
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace ac::sevenzip {
namespace {
bool ends(const std::string& s,const char* suffix) { auto n = std::strlen(suffix); return s.size() > n && !s.compare(s.size()-n,n,suffix); }
}
Mode mode(const std::string& name) {
    for (auto s : {".tar.xz",".txz",".tar.bz2",".tbz2",".tbz"}) if (ends(name,s)) return Mode::TarStream;
    for (auto s : {".7z",".rar",".cab",".iso",".arj",".lzh",".lha",".wim"}) if (ends(name,s)) return Mode::Archive;
    return Mode::None;
}
}

#ifndef _WIN32
namespace ac::sevenzip {
static const char* unavailable = "7z, RAR, CAB, ISO, TAR.XZ and TAR.BZ2 archives use 7-Zip's 7z.dll, which is only supported on Windows so far";
std::vector<Item> list(ReadSource&,const std::string&,const Cancel&) { throw std::runtime_error(unavailable); }
void extract(ReadSource&,const std::string&,uint32_t,const Cancel&,const std::function<void(const uint8_t*,size_t)>&) { throw std::runtime_error(unavailable); }
std::string library(std::string* why) { if (why) *why = unavailable; return {}; }
}
#else
#include <windows.h>
#include <oleauto.h>

namespace ac::sevenzip {
namespace {
// ---- 7-Zip COM-style interfaces (IArchive.h / IStream.h / IProgress.h / IPassword.h ABI).
// IIDs are {23170F69-40C1-278A-0000-00gg00ii0000}; format CLSIDs {23170F69-40C1-278A-1000-000110ff0000}.
constexpr GUID iid(uint8_t group,uint8_t id) { return {0x23170F69,0x40C1,0x278A,{0,0,0,group,0,id,0,0}}; }
constexpr GUID format(uint8_t id) { return {0x23170F69,0x40C1,0x278A,{0x10,0,0,0x01,0x10,id,0,0}}; }
const GUID IID_ISequentialInStream = iid(3,0x01), IID_ISequentialOutStream = iid(3,0x02), IID_IInStream = iid(3,0x03);
const GUID IID_IProgress = iid(0,0x05), IID_ICryptoGetTextPassword = iid(5,0x10);
const GUID IID_IArchiveOpenCallback = iid(6,0x10), IID_IArchiveExtractCallback = iid(6,0x20), IID_IInArchive = iid(6,0x60);

struct ISequentialInStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Read(void* data,UINT32 size,UINT32* processed) = 0; };
struct IInStream : ISequentialInStream { virtual HRESULT STDMETHODCALLTYPE Seek(INT64 offset,UINT32 origin,UINT64* position) = 0; };
struct ISequentialOutStream : IUnknown { virtual HRESULT STDMETHODCALLTYPE Write(const void* data,UINT32 size,UINT32* processed) = 0; };
struct IProgress7 : IUnknown { virtual HRESULT STDMETHODCALLTYPE SetTotal(UINT64) = 0; virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64*) = 0; };
struct IArchiveOpenCallback : IUnknown { virtual HRESULT STDMETHODCALLTYPE SetTotal(const UINT64*,const UINT64*) = 0; virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64*,const UINT64*) = 0; };
struct IArchiveExtractCallback : IProgress7 {
    virtual HRESULT STDMETHODCALLTYPE GetStream(UINT32 index,ISequentialOutStream** out,INT32 mode) = 0;
    virtual HRESULT STDMETHODCALLTYPE PrepareOperation(INT32 mode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOperationResult(INT32 result) = 0;
};
struct ICryptoGetTextPassword : IUnknown { virtual HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR* password) = 0; };
struct IInArchive : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Open(IInStream*,const UINT64* maxCheckStart,IArchiveOpenCallback*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProperty(UINT32 index,PROPID,PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Extract(const UINT32* indices,UINT32 count,INT32 test,IArchiveExtractCallback*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID,PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(UINT32,BSTR*,PROPID*,VARTYPE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(UINT32,BSTR*,PROPID*,VARTYPE*) = 0;
};
enum : PROPID { kpidPath = 3, kpidIsDir = 6, kpidSize = 7, kpidMTime = 12, kpidEncrypted = 15, kpidIsAnti = 21 };
enum : INT32 { kExtract = 0 };
using CreateObjectFn = HRESULT (WINAPI*)(const GUID* clsid,const GUID* iid,void** out);

// ---- library loading: 7z.dll next to aicmd.exe first, then an installed 7-Zip.
struct Library { HMODULE module = nullptr; CreateObjectFn create = nullptr; std::wstring path; std::string why; };
const Library& lib() {
    static Library l; static std::once_flag once;
    std::call_once(once,[]{
        std::vector<std::wstring> candidates;
        wchar_t self[MAX_PATH*4]{}; DWORD n = GetModuleFileNameW(nullptr,self,MAX_PATH*4);
        if (n) { std::wstring dir(self,n); dir.resize(dir.find_last_of(L"\\/")+1); candidates.push_back(dir+L"7z.dll"); }
        for (HKEY root : {HKEY_LOCAL_MACHINE,HKEY_CURRENT_USER}) for (auto value : {L"Path64",L"Path"}) {
            wchar_t buffer[MAX_PATH*2]{}; DWORD size = sizeof(buffer);
            if (RegGetValueW(root,L"SOFTWARE\\7-Zip",value,RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,buffer,&size) == ERROR_SUCCESS && buffer[0]) {
                std::wstring p = buffer; if (p.back() != L'\\') p += L'\\'; candidates.push_back(p+L"7z.dll");
            }
        }
        if (auto pf = _wgetenv(L"ProgramFiles")) candidates.push_back(std::wstring(pf)+L"\\7-Zip\\7z.dll");
        for (const auto& c : candidates) {
            if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            HMODULE m = LoadLibraryExW(c.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!m) continue;
            auto f = reinterpret_cast<CreateObjectFn>(reinterpret_cast<void*>(GetProcAddress(m,"CreateObject")));
            if (!f) { FreeLibrary(m); continue; }
            l.module = m; l.create = f; l.path = c; return;
        }
        l.why = "7z, RAR, CAB, ISO, TAR.XZ and TAR.BZ2 archives need 7-Zip's 7z.dll: put it next to aicmd.exe or install 7-Zip from https://www.7-zip.org";
    });
    return l;
}
std::string utf8(const wchar_t* w,int n = -1) {
    if (!w) return {};
    int size = WideCharToMultiByte(CP_UTF8,0,w,n,nullptr,0,nullptr,nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0),'\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8,0,w,n,out.data(),size,nullptr,nullptr);
    if (n < 0 && !out.empty()) out.pop_back();
    return out;
}

// Objects below live on the caller's stack and outlive the archive, so reference counts are not
// used for lifetime; 7-Zip still calls AddRef/Release in pairs.
struct InStream final : IInStream {
    InStream(ReadSource& s,const Cancel& c) : source(s), cancel(c) {}
    ReadSource& source; const Cancel& cancel; uint64_t position = 0; std::mutex lock;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override {
        if (IsEqualGUID(riid,IID_IUnknown) || IsEqualGUID(riid,IID_ISequentialInStream) || IsEqualGUID(riid,IID_IInStream)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Read(void* data,UINT32 size,UINT32* processed) override {
        std::lock_guard<std::mutex> guard(lock);
        if (processed) *processed = 0;
        if (cancel && cancel->load()) return E_ABORT;
        try {
            if (!size || position >= source.size()) return S_OK;
            auto part = source.read_at(position,size);
            std::memcpy(data,part.data(),part.size()); position += part.size();
            if (processed) *processed = static_cast<UINT32>(part.size());
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE Seek(INT64 offset,UINT32 origin,UINT64* out) override {
        std::lock_guard<std::mutex> guard(lock);
        int64_t base = origin == 0 ? 0 : origin == 1 ? static_cast<int64_t>(position) : origin == 2 ? static_cast<int64_t>(source.size()) : -1;
        if (base < 0) return STG_E_INVALIDFUNCTION;
        if (base+offset < 0) return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
        position = static_cast<uint64_t>(base+offset); if (out) *out = position; return S_OK;
    }
};
struct Password final : ICryptoGetTextPassword {
    bool asked = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override { *out = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CryptoGetTextPassword(BSTR*) override { asked = true; return E_ABORT; }
};
struct OpenCallback final : IArchiveOpenCallback {
    explicit OpenCallback(const Cancel& c) : cancel(c) {}
    const Cancel& cancel; Password password;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override {
        if (IsEqualGUID(riid,IID_IUnknown) || IsEqualGUID(riid,IID_IArchiveOpenCallback)) { *out = this; return S_OK; }
        if (IsEqualGUID(riid,IID_ICryptoGetTextPassword)) { *out = &password; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetTotal(const UINT64*,const UINT64*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64*,const UINT64*) override { return cancel && cancel->load() ? E_ABORT : S_OK; }
};
struct OutStream final : ISequentialOutStream {
    OutStream(const Cancel& c,const std::function<void(const uint8_t*,size_t)>& s) : cancel(c), sink(s) {}
    const Cancel& cancel; const std::function<void(const uint8_t*,size_t)>& sink; std::exception_ptr error;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** out) override {
        if (IsEqualGUID(riid,IID_IUnknown) || IsEqualGUID(riid,IID_ISequentialOutStream)) { *out = this; return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Write(const void* data,UINT32 size,UINT32* processed) override {
        if (processed) *processed = 0;
        if (error) return E_ABORT;
        if (cancel && cancel->load()) return E_ABORT;
        // Exceptions must not cross into 7z.dll: keep the first one and rethrow after Extract returns.
        try { sink(static_cast<const uint8_t*>(data),size); } catch (...) { error = std::current_exception(); return E_ABORT; }
        if (processed) *processed = size;
        return S_OK;
    }
};
struct ExtractCallback final : IArchiveExtractCallback {
    ExtractCallback(const Cancel& c,OutStream& o,uint32_t i) : cancel(c), out(o), index(i) {}
    const Cancel& cancel; OutStream& out; uint32_t index; Password password; INT32 result = -1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,void** o) override {
        if (IsEqualGUID(riid,IID_IUnknown) || IsEqualGUID(riid,IID_IProgress) || IsEqualGUID(riid,IID_IArchiveExtractCallback)) { *o = this; return S_OK; }
        if (IsEqualGUID(riid,IID_ICryptoGetTextPassword)) { *o = &password; return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetTotal(UINT64) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UINT64*) override { return cancel && cancel->load() ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE GetStream(UINT32 i,ISequentialOutStream** stream,INT32 mode) override {
        *stream = mode == kExtract && i == index ? &out : nullptr; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PrepareOperation(INT32) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetOperationResult(INT32 r) override { result = r; return S_OK; }
};

std::vector<GUID> formats_for(const std::string& name) {
    if (ends(name,".7z")) return {format(0x07)};
    if (ends(name,".rar")) return {format(0xCC),format(0x03)};
    if (ends(name,".cab")) return {format(0x08)};
    if (ends(name,".iso")) return {format(0xE7),format(0xE0)};
    if (ends(name,".arj")) return {format(0x04)};
    if (ends(name,".lzh") || ends(name,".lha")) return {format(0x06)};
    if (ends(name,".wim")) return {format(0xE6)};
    if (ends(name,".tar.xz") || ends(name,".txz")) return {format(0x0C)};
    if (ends(name,".tar.bz2") || ends(name,".tbz2") || ends(name,".tbz")) return {format(0x02)};
    return {};
}
// An opened archive; closes and releases on scope exit (before the stack callbacks die).
struct Opened {
    IInArchive* archive = nullptr;
    ~Opened() { if (archive) { archive->Close(); archive->Release(); } }
};
void open(Opened& opened,InStream& in,const std::string& name,const Cancel& cancel) {
    const auto& l = lib();
    if (!l.create) throw std::runtime_error(l.why);
    auto formats = formats_for(name);
    if (formats.empty()) throw std::runtime_error("Not a 7-Zip archive format");
    bool password = false;
    for (const auto& clsid : formats) {
        IInArchive* archive = nullptr;
        if (l.create(&clsid,&IID_IInArchive,reinterpret_cast<void**>(&archive)) != S_OK || !archive) continue;
        OpenCallback callback(cancel); UINT64 maxCheck = 1u << 23;
        in.position = 0;
        HRESULT hr = archive->Open(&in,&maxCheck,&callback);
        if (hr == S_OK) { opened.archive = archive; return; }
        password = password || callback.password.asked;
        archive->Close(); archive->Release();
        if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
    }
    if (password) throw std::runtime_error("Archive headers are encrypted; password-protected archives are not supported");
    throw std::runtime_error("7-Zip cannot open this archive (damaged or unsupported variant)");
}
bool flag(IInArchive* a,UINT32 i,PROPID id) {
    PROPVARIANT v; PropVariantInit(&v); bool out = false;
    if (a->GetProperty(i,id,&v) == S_OK && v.vt == VT_BOOL) out = v.boolVal != VARIANT_FALSE;
    PropVariantClear(&v); return out;
}
}

std::vector<Item> list(ReadSource& source,const std::string& name,const Cancel& cancel) {
    InStream in(source,cancel); Opened opened; open(opened,in,name,cancel);
    auto* a = opened.archive;
    UINT32 count = 0; if (a->GetNumberOfItems(&count) != S_OK) throw std::runtime_error("7-Zip cannot read the archive directory");
    std::vector<Item> out; out.reserve(count);
    for (UINT32 i = 0; i < count; ++i) {
        if ((i & 1023) == 0 && cancel && cancel->load()) throw std::runtime_error("Cancelled");
        if (flag(a,i,kpidIsAnti)) continue;
        Item item; item.index = i; item.directory = flag(a,i,kpidIsDir); item.encrypted = flag(a,i,kpidEncrypted);
        PROPVARIANT v; PropVariantInit(&v);
        if (a->GetProperty(i,kpidPath,&v) == S_OK && v.vt == VT_BSTR) item.path = utf8(v.bstrVal,static_cast<int>(SysStringLen(v.bstrVal)));
        PropVariantClear(&v);
        if (a->GetProperty(i,kpidSize,&v) == S_OK) item.size = v.vt == VT_UI8 ? v.uhVal.QuadPart : v.vt == VT_UI4 ? v.ulVal : 0;
        PropVariantClear(&v);
        if (a->GetProperty(i,kpidMTime,&v) == S_OK && v.vt == VT_FILETIME) {
            uint64_t t = (static_cast<uint64_t>(v.filetime.dwHighDateTime) << 32) | v.filetime.dwLowDateTime;
            if (t > 116444736000000000ull) item.modified = static_cast<int64_t>((t - 116444736000000000ull)/10000000ull);
        }
        PropVariantClear(&v);
        out.push_back(std::move(item));
    }
    return out;
}
void extract(ReadSource& source,const std::string& name,uint32_t index,const Cancel& cancel,const std::function<void(const uint8_t*,size_t)>& sink) {
    InStream in(source,cancel); Opened opened; open(opened,in,name,cancel);
    OutStream out(cancel,sink); ExtractCallback callback(cancel,out,index);
    HRESULT hr = opened.archive->Extract(&index,1,0,&callback);
    if (out.error) std::rethrow_exception(out.error);
    if (cancel && cancel->load()) throw std::runtime_error("Cancelled");
    if (callback.password.asked) throw std::runtime_error("Encrypted archive members are not supported");
    if (hr != S_OK) throw std::runtime_error("7-Zip extraction failed (error 0x"+[&]{ char b[16]; snprintf(b,sizeof b,"%08lX",static_cast<unsigned long>(hr)); return std::string(b); }()+")");
    switch (callback.result) {
    case 0: return;
    case 1: throw std::runtime_error("Unsupported compression method in archive");
    case 2: throw std::runtime_error("Archive data error (damaged archive)");
    case 3: throw std::runtime_error("Archive member CRC mismatch");
    case 5: throw std::runtime_error("Archive is truncated");
    case 9: throw std::runtime_error("Encrypted archive members are not supported");
    default: throw std::runtime_error("7-Zip reported error "+std::to_string(callback.result)+" for this member");
    }
}
std::string library(std::string* why) {
    const auto& l = lib();
    if (!l.create) { if (why) *why = l.why; return {}; }
    return utf8(l.path.c_str());
}
}
#endif
