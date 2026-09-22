#include "png.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
namespace av {
using Microsoft::WRL::ComPtr;
static void check(HRESULT h) {if (FAILED(h)) throw std::runtime_error("PNG encoding failed ("+std::to_string(static_cast<unsigned long>(h))+")");}
std::string preview_png(const Pixels& pixels,int& width,int& height) {
    const double scale = std::min(1.0,768.0/std::max(pixels.width,pixels.height));
    width = std::max(1,static_cast<int>(pixels.width*scale)); height = std::max(1,static_cast<int>(pixels.height*scale));
    std::vector<unsigned char> resized(static_cast<size_t>(width)*height*4);
    for (int y = 0;y < height;++y) for (int x = 0;x < width;++x) {
        auto source = pixels.bgra.data()+(static_cast<size_t>(y)*pixels.height/height*pixels.width+static_cast<size_t>(x)*pixels.width/width)*4;
        std::copy_n(source,4,resized.data()+(static_cast<size_t>(y)*width+x)*4);
    }
    ComPtr<IWICImagingFactory> factory; check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IStream> stream; check(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder; check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)); check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame,nullptr)); check(frame->Initialize(nullptr)); check(frame->SetSize(width,height));
    auto format = GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG pixel format unsupported");
    check(frame->WritePixels(height,width*4,static_cast<UINT>(resized.size()),resized.data())); check(frame->Commit()); check(encoder->Commit());
    STATSTG stat{}; check(stream->Stat(&stat,STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 3*1024*1024) throw std::runtime_error("Encoded preview exceeds response budget");
    LARGE_INTEGER zero{}; check(stream->Seek(zero,STREAM_SEEK_SET,nullptr)); std::vector<unsigned char> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0; check(stream->Read(bytes.data(),static_cast<ULONG>(bytes.size()),&read)); if (read != bytes.size()) throw std::runtime_error("PNG stream truncated");
    const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string encoded; encoded.reserve((bytes.size()+2)/3*4);
    for (size_t i = 0;i < bytes.size();i += 3) {
        uint32_t n = static_cast<uint32_t>(bytes[i])<<16; if (i+1 < bytes.size()) n |= static_cast<uint32_t>(bytes[i+1])<<8; if (i+2 < bytes.size()) n |= bytes[i+2];
        encoded += table[n>>18]; encoded += table[(n>>12)&63]; encoded += i+1 < bytes.size() ? table[(n>>6)&63] : '='; encoded += i+2 < bytes.size() ? table[n&63] : '=';
    }
    return encoded;
}
}
