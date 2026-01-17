#include "ThumbnailProvider.h"

#include "DecodeTypes.h"
#include "ImageCore.h"
#include "ImageDecodeDispatcher.h"
#include "ImageFormatDetector.h"
#include "ImageRequest.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <new>

#pragma comment(lib, "shlwapi.lib")

namespace
{
    const CLSID CLSID_Ficture2ThumbnailProvider =
        { 0x8b0a3d42, 0x7022, 0x4e35, { 0xb4, 0x5f, 0x73, 0x21, 0xb3, 0xe9, 0x3c, 0x16 } };

    // IInitializeWithStream GUID
    constexpr wchar_t kThumbnailProviderKey[] = L"{b824b49d-22ac-4161-ac8a-9916e8fa3f7f}";
    std::atomic<long> g_moduleRefCount { 0 };
    HMODULE g_moduleHandle = nullptr;
    std::once_flag g_decoderInitFlag;

    class ClassFactory final : public IClassFactory
    {
    public:
        ClassFactory()
        {
            g_moduleRefCount.fetch_add(1);
        }

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr)
            {
                return E_POINTER;
            }

            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
            {
                *ppv = static_cast<IClassFactory*>(this);
                AddRef();
                return S_OK;
            }

            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        IFACEMETHODIMP_(ULONG) AddRef() override
        {
            return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
        }

        IFACEMETHODIMP_(ULONG) Release() override
        {
            const ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
            if (ref == 0)
            {
                delete this;
            }
            return ref;
        }

        IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
        {
            if (ppv == nullptr)
            {
                return E_POINTER;
            }

            if (outer != nullptr)
            {
                return CLASS_E_NOAGGREGATION;
            }

            auto* provider = new (std::nothrow) Ficture2ThumbnailProvider();
            if (!provider)
            {
                return E_OUTOFMEMORY;
            }

            const HRESULT hr = provider->QueryInterface(riid, ppv);
            provider->Release();
            return hr;
        }

        IFACEMETHODIMP LockServer(BOOL lock) override
        {
            if (lock)
            {
                g_moduleRefCount.fetch_add(1);
            }
            else
            {
                g_moduleRefCount.fetch_sub(1);
            }
            return S_OK;
        }

    private:
        ~ClassFactory()
        {
            g_moduleRefCount.fetch_sub(1);
        }
        long m_refCount { 1 };
    };

    HRESULT RegisterServer(REFCLSID clsid, const wchar_t* description, const wchar_t* filePath)
    {
        if (!description || !filePath)
        {
            return E_INVALIDARG;
        }

        wchar_t clsidStr[64] {};
        if (StringFromGUID2(clsid, clsidStr, static_cast<int>(std::size(clsidStr))) == 0)
        {
            return E_FAIL;
        }

        wchar_t keyPath[256] {};
        swprintf_s(keyPath, L"CLSID\\%s", clsidStr);

        HKEY clsidKey = nullptr;
        if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &clsidKey, nullptr) != ERROR_SUCCESS)
        {
            return E_FAIL;
        }
        RegSetValueExW(clsidKey, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(description),
            static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t)));

        HKEY inprocKey = nullptr;
        if (RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inprocKey, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(inprocKey, nullptr, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(filePath),
                static_cast<DWORD>((wcslen(filePath) + 1) * sizeof(wchar_t)));
            const wchar_t threading[] = L"Apartment";
            RegSetValueExW(inprocKey, L"ThreadingModel", 0, REG_SZ,
                reinterpret_cast<const BYTE*>(threading),
                static_cast<DWORD>((wcslen(threading) + 1) * sizeof(wchar_t)));
            RegCloseKey(inprocKey);
        }
        RegCloseKey(clsidKey);

        wchar_t extKey[128] {};
        swprintf_s(extKey, L".dds\\ShellEx\\%s", kThumbnailProviderKey);
        HKEY shellExKey = nullptr;
        if (RegCreateKeyExW(HKEY_CLASSES_ROOT, extKey, 0, nullptr, 0, KEY_WRITE, nullptr, &shellExKey, nullptr) != ERROR_SUCCESS)
        {
            return E_FAIL;
        }
        RegSetValueExW(shellExKey, nullptr, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(clsidStr),
            static_cast<DWORD>((wcslen(clsidStr) + 1) * sizeof(wchar_t)));
        RegCloseKey(shellExKey);

        // Register as Approved Shell Extension
        HKEY approvedKey = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
            0, nullptr, 0, KEY_WRITE, nullptr, &approvedKey, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(approvedKey, clsidStr, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(description),
                static_cast<DWORD>((wcslen(description) + 1) * sizeof(wchar_t)));
            RegCloseKey(approvedKey);
        }

        return S_OK;
    }

    HRESULT UnregisterServer(REFCLSID clsid)
    {
        wchar_t clsidStr[64] {};
        if (StringFromGUID2(clsid, clsidStr, static_cast<int>(std::size(clsidStr))) == 0)
        {
            return E_FAIL;
        }

        wchar_t extKey[128] {};
        swprintf_s(extKey, L".dds\\ShellEx\\%s", kThumbnailProviderKey);
        RegDeleteTreeW(HKEY_CLASSES_ROOT, extKey);

        wchar_t clsidKey[256] {};
        swprintf_s(clsidKey, L"CLSID\\%s", clsidStr);
        RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey);

        // Remove from Approved Shell Extensions
        HKEY approvedKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
            0, KEY_WRITE, &approvedKey) == ERROR_SUCCESS)
        {
            RegDeleteValueW(approvedKey, clsidStr);
            RegCloseKey(approvedKey);
        }

        return S_OK;
    }

    HRESULT CreateBitmapFromDecodedImage(const ImageCore::DecodedImage& image, HBITMAP* outBitmap)
    {
        if (outBitmap == nullptr || !image.blocks || image.dxgiFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
        {
            return E_INVALIDARG;
        }

        const uint32_t width = image.width;
        const uint32_t height = image.height;
        if (width == 0 || height == 0 || image.rowPitchBytes == 0)
        {
            return E_FAIL;
        }

        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbm || bits == nullptr)
        {
            if (hbm)
            {
                DeleteObject(hbm);
            }
            return E_FAIL;
        }

        const uint8_t* src = image.blocks->data();
        auto* dst = static_cast<uint8_t*>(bits);
        const uint32_t rowBytes = width * 4u;
        for (uint32_t y = 0; y < height; ++y)
        {
            memcpy(dst + (rowBytes * y), src + (image.rowPitchBytes * y), rowBytes);
        }

        *outBitmap = hbm;
        return S_OK;
    }
}

Ficture2ThumbnailProvider::Ficture2ThumbnailProvider()
{
    g_moduleRefCount.fetch_add(1);
}

Ficture2ThumbnailProvider::~Ficture2ThumbnailProvider()
{
    g_moduleRefCount.fetch_sub(1);
}

IFACEMETHODIMP Ficture2ThumbnailProvider::QueryInterface(REFIID riid, void** ppv)
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IThumbnailProvider))
    {
        *ppv = static_cast<IThumbnailProvider*>(this);
        AddRef();
        return S_OK;
    }

    if (IsEqualIID(riid, IID_IInitializeWithStream))
    {
        *ppv = static_cast<IInitializeWithStream*>(this);
        AddRef();
        return S_OK;
    }

    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) Ficture2ThumbnailProvider::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(reinterpret_cast<long*>(&m_refCount)));
}

IFACEMETHODIMP_(ULONG) Ficture2ThumbnailProvider::Release()
{
    const ULONG ref = static_cast<ULONG>(InterlockedDecrement(reinterpret_cast<long*>(&m_refCount)));
    if (ref == 0)
    {
        delete this;
    }
    return ref;
}

IFACEMETHODIMP Ficture2ThumbnailProvider::Initialize(IStream* stream, DWORD grfMode)
{
    UNREFERENCED_PARAMETER(grfMode);
    
    if (!stream)
    {
        return E_INVALIDARG;
    }

    // Get stream size
    STATSTG stat {};
    HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr))
    {
        return hr;
    }

    const ULONGLONG fileSize = stat.cbSize.QuadPart;
    if (fileSize == 0 || fileSize > 100 * 1024 * 1024) // Max 100MB
    {
        return E_FAIL;
    }

    // Read stream data
    m_fileData.resize(static_cast<size_t>(fileSize));
    ULONG bytesRead = 0;
    hr = stream->Read(m_fileData.data(), static_cast<ULONG>(fileSize), &bytesRead);
    if (FAILED(hr))
    {
        m_fileData.clear();
        return hr;
    }

    if (bytesRead != fileSize)
    {
        m_fileData.clear();
        return E_FAIL;
    }

    return S_OK;
}

IFACEMETHODIMP Ficture2ThumbnailProvider::GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha)
{
    if (!phbmp || !pdwAlpha)
    {
        return E_POINTER;
    }

    if (m_fileData.empty())
    {
        return E_FAIL;
    }

    std::call_once(g_decoderInitFlag, []()
    {
        ImageCore::RegisterBuiltInDecoders();
    });

    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    ImageCore::ImageRequest request {};
    request.source = L"thumbnail.dds";
    request.purpose = ImageCore::ImagePurpose::Thumbnail;
    request.targetSize = { static_cast<float>(cx), static_cast<float>(cx) };
    request.srgb = true;
    request.allowGpuCompressedDDS = false;

    ImageCore::DecodeInput input {};
    input.bytes = std::span<const uint8_t>(m_fileData.data(), m_fileData.size());
    input.header = std::span<const uint8_t>(m_fileData.data(), std::min<size_t>(m_fileData.size(), 512));

    ImageCore::ImageDecodeDispatcher dispatcher {};
    ImageCore::PipelineResult result = dispatcher.Decode(request, wicFactory.Get(), input);
    if (FAILED(result.hr))
    {
        return result.hr;
    }

    HBITMAP hbm = nullptr;
    hr = CreateBitmapFromDecodedImage(result.image, &hbm);
    if (FAILED(hr))
    {
        return hr;
    }

    *phbmp = hbm;
    *pdwAlpha = WTSAT_ARGB;
    return S_OK;
}

STDAPI DllCanUnloadNow()
{
    const long refCount = g_moduleRefCount.load();
    return (refCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!IsEqualCLSID(rclsid, CLSID_Ficture2ThumbnailProvider))
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory)
    {
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer()
{
    if (!g_moduleHandle)
    {
        return E_FAIL;
    }

    wchar_t modulePath[MAX_PATH] {};
    if (GetModuleFileNameW(g_moduleHandle, modulePath, static_cast<DWORD>(std::size(modulePath))) == 0)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return RegisterServer(CLSID_Ficture2ThumbnailProvider, L"FICture2 Thumbnail Provider", modulePath);
}

STDAPI DllUnregisterServer()
{
    return UnregisterServer(CLSID_Ficture2ThumbnailProvider);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_moduleHandle = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
