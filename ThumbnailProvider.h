#pragma once

#include <windows.h>
#include <thumbcache.h>
#include <shobjidl_core.h>
#include <string>
#include <vector>

class Ficture2ThumbnailProvider final : public IThumbnailProvider, public IInitializeWithStream
{
public:
    Ficture2ThumbnailProvider();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP Initialize(IStream* stream, DWORD grfMode) override;
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override;

private:
    ~Ficture2ThumbnailProvider();

    ULONG m_refCount { 1 };
    std::vector<uint8_t> m_fileData {};
};
