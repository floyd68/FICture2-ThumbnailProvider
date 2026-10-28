// ThumbnailProvider unit tests (FICture2-ThumbnailProvider #1).
//
// The provider objects come from the ThumbnailProviderCore OBJECT library, the
// same objects the DLL links, so this executable and the provider code share
// one ImageCore copy (and one thread_local WIC slot). The provider is created
// directly: no regsvr32, no registry, no DllGetClassObject. Every provider call
// runs on a fresh STA thread, as on an Explorer thumbnail thread. Fixtures are
// generated in the test.
//
// Usage: ThumbnailProviderTests [--list | <test name prefix>]

#include "ThumbnailProvider.h"

#include "DecodeTypes.h"
#include "ImageCore.h"
#include "ImageDecodeDispatcher.h"
#include "ImageFormatDetector.h"
#include "ImageRequest.h"

#include <DirectXTex.h>
#include <objbase.h>
#include <windows.h>
#include <winerror.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// ---------- Minimal harness ----------

namespace
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* name { nullptr };
        TestFn fn { nullptr };
    };

    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> registry;
        return registry;
    }

    struct Registrar
    {
        Registrar(const char* name, TestFn fn)
        {
            Registry().push_back({ name, fn });
        }
    };

    struct RequireFailed
    {
    };

    int g_failuresInCurrentTest = 0;

    void ReportFailure(const char* file, int line, const char* expression)
    {
        ++g_failuresInCurrentTest;
        std::fprintf(stderr, "  %s(%d): check failed: %s\n", file, line, expression);
    }
}

#define TP_TEST(name) \
    static void name(); \
    static const Registrar name##_registrar(#name, &name); \
    static void name()

#define TP_CHECK(expr) \
    do { \
        if (!(expr)) \
        { \
            ReportFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define TP_REQUIRE(expr) \
    do { \
        if (!(expr)) \
        { \
            ReportFailure(__FILE__, __LINE__, #expr); \
            throw RequireFailed {}; \
        } \
    } while (0)

// ---------- Fixtures ----------

namespace
{
    constexpr size_t kWidth = 256;
    constexpr size_t kHeight = 128;

    // Four solid quadrants (top-left, top-right, bottom-left, bottom-right),
    // straight RGBA. No alpha is 0: DirectXTex's resize premultiplies, which
    // drops the color of fully transparent texels.
    struct Rgba
    {
        uint8_t r, g, b, a;
    };
    constexpr Rgba kQuadrants[4] =
    {
        { 200, 40, 40, 255 },
        { 40, 200, 40, 160 },
        { 40, 40, 200, 80 },
        { 230, 230, 230, 48 },
    };

    int QuadrantAt(size_t x, size_t y, size_t width, size_t height)
    {
        return (x >= width / 2 ? 1 : 0) + (y >= height / 2 ? 2 : 0);
    }

    // DirectXTex encodes through its process-global WIC factory, so COM stays
    // initialized on the main thread for the process.
    void EnsureProcessCom()
    {
        static const HRESULT s_hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        TP_REQUIRE(SUCCEEDED(s_hrCom));
    }

    std::vector<uint8_t> ToBytes(const DirectX::Blob& blob)
    {
        const auto* bytes = static_cast<const uint8_t*>(blob.GetBufferPointer());
        return std::vector<uint8_t>(bytes, bytes + blob.GetBufferSize());
    }

    // The quadrant image as R8G8B8A8, alpha forced opaque unless `alpha`.
    DirectX::ScratchImage MakeQuadrants(bool alpha, size_t width = kWidth, size_t height = kHeight)
    {
        DirectX::ScratchImage image;
        TP_REQUIRE(SUCCEEDED(image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1)));
        const DirectX::Image* base = image.GetImage(0, 0, 0);
        TP_REQUIRE(base != nullptr);
        for (size_t y = 0; y < height; ++y)
        {
            uint8_t* row = base->pixels + y * base->rowPitch;
            for (size_t x = 0; x < width; ++x)
            {
                const Rgba& c = kQuadrants[QuadrantAt(x, y, width, height)];
                row[x * 4 + 0] = c.r;
                row[x * 4 + 1] = c.g;
                row[x * 4 + 2] = c.b;
                row[x * 4 + 3] = alpha ? c.a : 255;
            }
        }
        return image;
    }

    // A DDS of the quadrant image with a full mip chain, stored as `format`
    // (a BC format, or B8G8R8A8_UNORM). `outMip0` receives the decoded texels of
    // mip 0 as stored (after block compression), as R8G8B8A8.
    std::vector<uint8_t> MakeDds(DXGI_FORMAT format, bool alpha, DirectX::ScratchImage* outMip0 = nullptr)
    {
        EnsureProcessCom();
        const DirectX::ScratchImage base = MakeQuadrants(alpha);
        DirectX::ScratchImage mips;
        TP_REQUIRE(SUCCEEDED(DirectX::GenerateMipMaps(*base.GetImage(0, 0, 0),
            DirectX::TEX_FILTER_BOX | DirectX::TEX_FILTER_FORCE_NON_WIC, 0, mips)));

        DirectX::ScratchImage stored;
        if (DirectX::IsCompressed(format))
        {
            TP_REQUIRE(SUCCEEDED(DirectX::Compress(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(),
                format, DirectX::TEX_COMPRESS_BC7_QUICK, DirectX::TEX_THRESHOLD_DEFAULT, stored)));
        }
        else
        {
            TP_REQUIRE(SUCCEEDED(DirectX::Convert(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(),
                format, DirectX::TEX_FILTER_FORCE_NON_WIC, DirectX::TEX_THRESHOLD_DEFAULT, stored)));
        }

        if (outMip0 != nullptr)
        {
            const DirectX::Image* mip0 = stored.GetImage(0, 0, 0);
            TP_REQUIRE(mip0 != nullptr);
            if (DirectX::IsCompressed(format))
            {
                TP_REQUIRE(SUCCEEDED(DirectX::Decompress(*mip0, DXGI_FORMAT_R8G8B8A8_UNORM, *outMip0)));
            }
            else
            {
                TP_REQUIRE(SUCCEEDED(DirectX::Convert(*mip0, DXGI_FORMAT_R8G8B8A8_UNORM,
                    DirectX::TEX_FILTER_FORCE_NON_WIC, DirectX::TEX_THRESHOLD_DEFAULT, *outMip0)));
            }
        }

        DirectX::Blob blob;
        TP_REQUIRE(SUCCEEDED(DirectX::SaveToDDSMemory(stored.GetImages(), stored.GetImageCount(),
            stored.GetMetadata(), DirectX::DDS_FLAGS_NONE, blob)));
        return ToBytes(blob);
    }

    // "DDS " magic followed by a header DirectXTex rejects (size field 0).
    std::vector<uint8_t> CorruptDds()
    {
        std::vector<uint8_t> bytes(4 + 124 + 256, 0);
        std::memcpy(bytes.data(), "DDS ", 4);
        return bytes;
    }

    std::vector<uint8_t> MakePng()
    {
        EnsureProcessCom();
        const DirectX::ScratchImage image = MakeQuadrants(false, 16, 8);
        DirectX::Blob blob;
        TP_REQUIRE(SUCCEEDED(DirectX::SaveToWICMemory(*image.GetImage(0, 0, 0), DirectX::WIC_FLAGS_NONE,
            DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), blob)));
        return ToBytes(blob);
    }

    // ---------- Test stream ----------

    // An in-memory IStream whose Read returns at most `maxChunk` bytes per
    // call, reports `reportedSize` from Stat, serves zeros past the stored
    // bytes up to `virtualSize`, and fails with `failHr` once `failAt` bytes
    // were read.
    class TestStream final : public IStream
    {
    public:
        explicit TestStream(std::vector<uint8_t> data)
            : m_data(std::move(data))
            , m_reportedSize(m_data.size())
            , m_virtualSize(m_data.size())
        {
        }

        ULONG maxChunk { ULONG_MAX };
        HRESULT shortReadHr { S_OK };   // returned with a short (chunked) read
        ULONGLONG failAt { ULLONG_MAX };
        HRESULT failHr { STG_E_READFAULT };
        ULONG readCalls { 0 };

        void SetReportedSize(ULONGLONG size)
        {
            m_reportedSize = size;
        }

        void SetVirtualSize(ULONGLONG size)
        {
            m_virtualSize = size;
        }

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv == nullptr)
            {
                return E_POINTER;
            }
            if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ISequentialStream) || IsEqualIID(riid, IID_IStream))
            {
                *ppv = static_cast<IStream*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        // Stack-owned in the tests.
        IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
        IFACEMETHODIMP_(ULONG) Release() override { return 1; }

        IFACEMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override
        {
            ++readCalls;
            if (pcbRead != nullptr)
            {
                *pcbRead = 0;
            }
            if (m_position >= failAt)
            {
                return failHr;
            }
            const ULONGLONG available = (m_position < m_virtualSize) ? m_virtualSize - m_position : 0;
            ULONGLONG count = (std::min<ULONGLONG>)(cb, available);
            count = (std::min<ULONGLONG>)(count, maxChunk);
            if (failAt != ULLONG_MAX)
            {
                count = (std::min<ULONGLONG>)(count, failAt - m_position);
            }

            auto* out = static_cast<uint8_t*>(pv);
            for (ULONGLONG i = 0; i < count; ++i)
            {
                const ULONGLONG at = m_position + i;
                out[i] = (at < m_data.size()) ? m_data[static_cast<size_t>(at)] : 0;
            }
            m_position += count;
            if (pcbRead != nullptr)
            {
                *pcbRead = static_cast<ULONG>(count);
            }
            if (count == cb)
            {
                return S_OK;
            }
            return (count == available) ? S_FALSE : shortReadHr;
        }

        IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
        IFACEMETHODIMP Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) override { return E_NOTIMPL; }
        IFACEMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
        IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
        IFACEMETHODIMP Commit(DWORD) override { return S_OK; }
        IFACEMETHODIMP Revert() override { return E_NOTIMPL; }
        IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
        IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
        IFACEMETHODIMP Clone(IStream**) override { return E_NOTIMPL; }

        IFACEMETHODIMP Stat(STATSTG* stat, DWORD) override
        {
            if (stat == nullptr)
            {
                return E_POINTER;
            }
            *stat = {};
            stat->type = STGTY_STREAM;
            stat->cbSize.QuadPart = m_reportedSize;
            return S_OK;
        }

    private:
        std::vector<uint8_t> m_data;
        ULONGLONG m_reportedSize { 0 };
        ULONGLONG m_virtualSize { 0 };
        ULONGLONG m_position { 0 };
    };

    // ---------- Provider helpers ----------

    // Runs `body` on a new STA thread (an Explorer thumbnail thread is an STA)
    // and rethrows a failed TP_REQUIRE. The thread releases ImageCore's thread
    // resources itself before CoUninitialize, so a test that checks the
    // provider's release reads the state before this cleanup.
    void RunOnStaThread(const std::function<void()>& body)
    {
        EnsureProcessCom();
        bool requireFailed = false;
        std::thread worker([&]()
        {
            const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            try
            {
                TP_REQUIRE(SUCCEEDED(hrCom));
                body();
            }
            catch (const RequireFailed&)
            {
                requireFailed = true;
            }
            catch (const std::exception& ex)
            {
                ReportFailure(__FILE__, __LINE__, ex.what());
            }
            ImageCore::ImageDecodeDispatcher::ReleaseThreadResources();
            if (SUCCEEDED(hrCom))
            {
                CoUninitialize();
            }
        });
        worker.join();
        if (requireFailed)
        {
            throw RequireFailed {};
        }
    }

    // Owns one provider instance, used through its COM interfaces.
    class Provider final
    {
    public:
        Provider()
        {
            auto* provider = new Ficture2ThumbnailProvider();
            TP_REQUIRE(SUCCEEDED(provider->QueryInterface(IID_PPV_ARGS(&m_init))));
            TP_REQUIRE(SUCCEEDED(provider->QueryInterface(IID_PPV_ARGS(&m_thumb))));
            provider->Release();
        }

        ~Provider()
        {
            m_init->Release();
            m_thumb->Release();
        }

        Provider(const Provider&) = delete;
        Provider& operator=(const Provider&) = delete;

        HRESULT Initialize(IStream* stream)
        {
            return m_init->Initialize(stream, STGM_READ);
        }

        IThumbnailProvider* Thumb() const
        {
            return m_thumb;
        }

    private:
        IInitializeWithStream* m_init { nullptr };
        IThumbnailProvider* m_thumb { nullptr };
    };

    struct Bitmap
    {
        HRESULT hr { E_FAIL };
        bool nullOnFailure { false };
        WTS_ALPHATYPE alpha { WTSAT_UNKNOWN };
        int width { 0 };
        int height { 0 };
        std::vector<uint8_t> bgra {};  // top-down rows, width * 4 bytes each

        const uint8_t* At(int x, int y) const
        {
            return &bgra[(static_cast<size_t>(y) * width + x) * 4];
        }
    };

    Bitmap GetThumbnail(const Provider& provider, UINT cx)
    {
        Bitmap out {};
        HBITMAP hbm = reinterpret_cast<HBITMAP>(static_cast<uintptr_t>(0x1));  // must be reset
        WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
        out.hr = provider.Thumb()->GetThumbnail(cx, &hbm, &alpha);
        if (FAILED(out.hr))
        {
            out.nullOnFailure = (hbm == nullptr);
            return out;
        }
        TP_REQUIRE(hbm != nullptr);
        out.alpha = alpha;

        BITMAP info {};
        TP_REQUIRE(GetObjectW(hbm, sizeof(info), &info) == sizeof(info));
        TP_CHECK(info.bmBitsPixel == 32);
        out.width = info.bmWidth;
        out.height = info.bmHeight;

        BITMAPINFO bmi {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = out.width;
        bmi.bmiHeader.biHeight = -out.height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        out.bgra.resize(static_cast<size_t>(out.width) * out.height * 4);
        HDC dc = CreateCompatibleDC(nullptr);
        TP_REQUIRE(dc != nullptr);
        const int lines = GetDIBits(dc, hbm, 0, static_cast<UINT>(out.height), out.bgra.data(), &bmi, DIB_RGB_COLORS);
        DeleteDC(dc);
        DeleteObject(hbm);
        TP_REQUIRE(lines == out.height);
        return out;
    }

    // Initializes a provider from `bytes` (one Read) and gets one thumbnail.
    Bitmap ThumbnailOf(const std::vector<uint8_t>& bytes, UINT cx)
    {
        Provider provider;
        TestStream stream(bytes);
        TP_REQUIRE(SUCCEEDED(provider.Initialize(&stream)));
        return GetThumbnail(provider, cx);
    }

    bool Near(const uint8_t* bgra, int b, int g, int r, int a, int tolerance)
    {
        return std::abs(bgra[0] - b) <= tolerance && std::abs(bgra[1] - g) <= tolerance &&
            std::abs(bgra[2] - r) <= tolerance && std::abs(bgra[3] - a) <= tolerance;
    }

    // Checks the centre of each quadrant of `bitmap` against `expected(q)`
    // (B, G, R, A).
    void CheckQuadrants(const Bitmap& bitmap, const std::function<std::array<int, 4>(int)>& expected, int tolerance)
    {
        for (int q = 0; q < 4; ++q)
        {
            const int x = bitmap.width / 4 + ((q & 1) ? bitmap.width / 2 : 0);
            const int y = bitmap.height / 4 + ((q & 2) ? bitmap.height / 2 : 0);
            const std::array<int, 4> e = expected(q);
            const uint8_t* p = bitmap.At(x, y);
            if (!Near(p, e[0], e[1], e[2], e[3], tolerance))
            {
                std::fprintf(stderr, "  quadrant %d at %dx%d: BGRA %u,%u,%u,%u, expected %d,%d,%d,%d\n", q,
                    bitmap.width, bitmap.height, p[0], p[1], p[2], p[3], e[0], e[1], e[2], e[3]);
                TP_CHECK(false);
            }
        }
    }

    // Opaque quadrant colors (B, G, R, 255).
    std::array<int, 4> OpaqueQuadrant(int q)
    {
        const Rgba& c = kQuadrants[q];
        return { c.b, c.g, c.r, 255 };
    }

    bool IsWicError(HRESULT hr)
    {
        return HRESULT_FACILITY(hr) == FACILITY_WINCODEC_ERR;
    }
}

// ---------- Stream reading ----------

TP_TEST(Stream_ChunkedReadsSucceed)
{
    const std::vector<uint8_t> dds = MakeDds(DXGI_FORMAT_B8G8R8A8_UNORM, false);
    RunOnStaThread([&]()
    {
        const Bitmap whole = ThumbnailOf(dds, 64);
        TP_REQUIRE(SUCCEEDED(whole.hr));

        for (const ULONG chunk : { 1ul, 7ul, 4096ul })
        {
            for (const HRESULT shortReadHr : { S_OK, S_FALSE })
            {
                Provider provider;
                TestStream stream(dds);
                stream.maxChunk = chunk;
                stream.shortReadHr = shortReadHr;
                TP_CHECK(provider.Initialize(&stream) == S_OK);
                TP_CHECK(stream.readCalls >= (dds.size() + chunk - 1) / chunk);

                const Bitmap chunked = GetThumbnail(provider, 64);
                TP_CHECK(SUCCEEDED(chunked.hr));
                TP_CHECK(chunked.width == whole.width && chunked.height == whole.height);
                TP_CHECK(chunked.bgra == whole.bgra);
            }
        }
    });
}

TP_TEST(Stream_TruncatedStreamsFail)
{
    const std::vector<uint8_t> dds = MakeDds(DXGI_FORMAT_B8G8R8A8_UNORM, false);
    RunOnStaThread([&]()
    {
        for (const ULONG chunk : { 7ul, ULONG_MAX })
        {
            // Stat promises more bytes than the stream delivers.
            Provider provider;
            TestStream stream(dds);
            stream.maxChunk = chunk;
            stream.SetReportedSize(dds.size() + 100);
            const HRESULT hr = provider.Initialize(&stream);
            TP_CHECK(hr == HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));

            // No partial data is kept: GetThumbnail fails without a bitmap.
            const Bitmap bitmap = GetThumbnail(provider, 64);
            TP_CHECK(FAILED(bitmap.hr));
            TP_CHECK(bitmap.nullOnFailure);
        }

        // A failed re-initialization drops the earlier data as well.
        Provider provider;
        TestStream good(dds);
        TP_REQUIRE(SUCCEEDED(provider.Initialize(&good)));
        TestStream truncated(dds);
        truncated.SetReportedSize(dds.size() + 1);
        TP_CHECK(FAILED(provider.Initialize(&truncated)));
        TP_CHECK(FAILED(GetThumbnail(provider, 64).hr));
    });
}

TP_TEST(Stream_ReadFailuresFail)
{
    const std::vector<uint8_t> dds = MakeDds(DXGI_FORMAT_B8G8R8A8_UNORM, false);
    RunOnStaThread([&]()
    {
        for (const ULONGLONG failAt : { 0ull, 1000ull })
        {
            Provider provider;
            TestStream stream(dds);
            stream.maxChunk = 100;
            stream.failAt = failAt;
            stream.failHr = STG_E_READFAULT;
            TP_CHECK(provider.Initialize(&stream) == STG_E_READFAULT);

            const Bitmap bitmap = GetThumbnail(provider, 64);
            TP_CHECK(FAILED(bitmap.hr));
            TP_CHECK(bitmap.nullOnFailure);
        }
    });
}

TP_TEST(Stream_SizeLimitIsKept)
{
    constexpr ULONGLONG kLimit = 100ull * 1024ull * 1024ull;
    RunOnStaThread([&]()
    {
        {
            // Over the limit (and empty): rejected before any read.
            Provider provider;
            TestStream stream({ 1, 2, 3 });
            stream.SetReportedSize(kLimit + 1);
            stream.SetVirtualSize(kLimit + 1);
            TP_CHECK(FAILED(provider.Initialize(&stream)));
            TP_CHECK(stream.readCalls == 0);

            TestStream empty({});
            TP_CHECK(FAILED(provider.Initialize(&empty)));
            TP_CHECK(empty.readCalls == 0);
        }
        {
            // Exactly the limit is read in full.
            Provider provider;
            TestStream stream({ 'D', 'D', 'S', ' ' });
            stream.SetReportedSize(kLimit);
            stream.SetVirtualSize(kLimit);
            stream.maxChunk = 16u * 1024u * 1024u;
            TP_CHECK(provider.Initialize(&stream) == S_OK);
            TP_CHECK(stream.readCalls == 7);
        }
    });
}

// ---------- GetThumbnail ----------

TP_TEST(GetThumbnail_RepeatedCallsWithDifferentCx)
{
    const std::vector<uint8_t> dds = MakeDds(DXGI_FORMAT_B8G8R8A8_UNORM, false);
    RunOnStaThread([&]()
    {
        Provider provider;
        TestStream stream(dds);
        TP_REQUIRE(SUCCEEDED(provider.Initialize(&stream)));

        const Bitmap first = GetThumbnail(provider, 32);
        const Bitmap larger = GetThumbnail(provider, 96);
        const Bitmap again = GetThumbnail(provider, 32);
        const Bitmap oversized = GetThumbnail(provider, 512);
        for (const Bitmap* bitmap : { &first, &larger, &again, &oversized })
        {
            TP_REQUIRE(SUCCEEDED(bitmap->hr));
            TP_CHECK(bitmap->alpha == WTSAT_ARGB);
            CheckQuadrants(*bitmap, OpaqueQuadrant, 2);
        }
        TP_CHECK(first.width == 32 && first.height == 16);
        TP_CHECK(larger.width == 96 && larger.height == 48);
        // Never upscaled past the source.
        TP_CHECK(oversized.width == static_cast<int>(kWidth) && oversized.height == static_cast<int>(kHeight));
        TP_CHECK(again.width == first.width && again.height == first.height);
        TP_CHECK(again.bgra == first.bgra);
    });
}

TP_TEST(GetThumbnail_ReleasesThreadResources)
{
    const std::vector<uint8_t> png = MakePng();
    const std::vector<uint8_t> dds = MakeDds(DXGI_FORMAT_BC1_UNORM, false);
    const std::vector<uint8_t> corrupt = CorruptDds();

    RunOnStaThread([&]()
    {
        // Creates ImageCore's thread_local WIC factory on this thread, through
        // the same ImageCore copy the provider uses.
        const auto createThreadResource = [&]()
        {
            ImageCore::RegisterBuiltInDecoders();
            ImageCore::ImageRequest request(L"resource.png", ImageCore::ImagePurpose::FullResolution);
            ImageCore::DecodeInput input {};
            input.bytes = std::span<const uint8_t>(png.data(), png.size());
            input.header = std::span<const uint8_t>(png.data(),
                (std::min)(png.size(), ImageCore::ImageFormatDetector::kProbeSize));
            ImageCore::ImageDecodeDispatcher dispatcher;
            TP_REQUIRE(SUCCEEDED(dispatcher.Decode(request, input).hr));
            TP_REQUIRE(ImageCore::ImageDecodeDispatcher::HasThreadResources());
        };

        Provider provider;
        TestStream stream(dds);
        TP_REQUIRE(SUCCEEDED(provider.Initialize(&stream)));

        // Successful calls, twice on one instance.
        for (const UINT cx : { 64u, 128u })
        {
            createThreadResource();
            const Bitmap bitmap = GetThumbnail(provider, cx);
            TP_CHECK(SUCCEEDED(bitmap.hr));
            TP_CHECK(!ImageCore::ImageDecodeDispatcher::HasThreadResources());
        }

        // A call that fails in the decoder.
        Provider corruptProvider;
        TestStream corruptStream(corrupt);
        TP_REQUIRE(SUCCEEDED(corruptProvider.Initialize(&corruptStream)));
        createThreadResource();
        const Bitmap failed = GetThumbnail(corruptProvider, 64);
        TP_CHECK(FAILED(failed.hr));
        TP_CHECK(!ImageCore::ImageDecodeDispatcher::HasThreadResources());
    });
}

TP_TEST(GetThumbnail_CorruptDdsFailsWithoutWic)
{
    const std::vector<uint8_t> corrupt = CorruptDds();
    for (const UINT cx : { 96u, 256u })
    {
        RunOnStaThread([&]()
        {
            TP_REQUIRE(!ImageCore::ImageDecodeDispatcher::HasThreadResources());
            Provider provider;
            TestStream stream(corrupt);
            TP_REQUIRE(SUCCEEDED(provider.Initialize(&stream)));
            const Bitmap bitmap = GetThumbnail(provider, cx);
            TP_CHECK(FAILED(bitmap.hr));
            TP_CHECK(bitmap.nullOnFailure);
            // DirectXTex's failure, not a WIC codec's: the input never reached WIC.
            if (IsWicError(bitmap.hr))
            {
                std::fprintf(stderr, "  WIC error 0x%08lX\n", static_cast<unsigned long>(bitmap.hr));
            }
            TP_CHECK(!IsWicError(bitmap.hr));
            TP_CHECK(!ImageCore::ImageDecodeDispatcher::HasThreadResources());
        });
    }
}

// In-process stand-in for Explorer's Large icons (96) and Extra large icons
// (256) views: block-compressed DDS with and without alpha, and an
// uncompressed one with alpha. Block-compressed straight alpha is presented
// as data (opaque RGB); uncompressed alpha as coverage (premultiplied).
TP_TEST(GetThumbnail_DdsFormatsAtExplorerSizes)
{
    struct Case
    {
        const char* name;
        DXGI_FORMAT format;
        bool alpha;
    };
    const Case cases[] =
    {
        { "BC1", DXGI_FORMAT_BC1_UNORM, false },
        { "BC1 alpha", DXGI_FORMAT_BC1_UNORM, true },
        { "BC3", DXGI_FORMAT_BC3_UNORM, false },
        { "BC3 alpha", DXGI_FORMAT_BC3_UNORM, true },
        { "BC7", DXGI_FORMAT_BC7_UNORM, false },
        { "BC7 alpha", DXGI_FORMAT_BC7_UNORM, true },
        { "BGRA8 alpha", DXGI_FORMAT_B8G8R8A8_UNORM, true },
    };

    for (const Case& c : cases)
    {
        DirectX::ScratchImage mip0;
        const std::vector<uint8_t> dds = MakeDds(c.format, c.alpha, &mip0);
        const DirectX::Image* stored = mip0.GetImage(0, 0, 0);
        TP_REQUIRE(stored != nullptr);
        const bool compressed = DirectX::IsCompressed(c.format);

        // The stored texel at each quadrant centre, as the provider presents it.
        const auto expected = [&](int q) -> std::array<int, 4>
        {
            const size_t x = kWidth / 4 + ((q & 1) ? kWidth / 2 : 0);
            const size_t y = kHeight / 4 + ((q & 2) ? kHeight / 2 : 0);
            const uint8_t* rgba = stored->pixels + y * stored->rowPitch + x * 4;
            if (compressed)
            {
                return { rgba[2], rgba[1], rgba[0], 255 };
            }
            const auto premultiply = [&](uint8_t v) { return (v * rgba[3] + 127) / 255; };
            return { premultiply(rgba[2]), premultiply(rgba[1]), premultiply(rgba[0]), rgba[3] };
        };

        std::printf("  %s\n", c.name);
        RunOnStaThread([&]()
        {
            Provider provider;
            TestStream stream(dds);
            TP_REQUIRE(SUCCEEDED(provider.Initialize(&stream)));
            for (const UINT cx : { 96u, 256u })
            {
                const Bitmap bitmap = GetThumbnail(provider, cx);
                TP_REQUIRE(SUCCEEDED(bitmap.hr));
                TP_CHECK(bitmap.alpha == WTSAT_ARGB);
                TP_CHECK(bitmap.width == static_cast<int>(cx) && bitmap.height == static_cast<int>(cx / 2));
                CheckQuadrants(bitmap, expected, 4);
            }
        });
    }
}

int main(int argc, char** argv)
{
    const char* prefix = (argc > 1) ? argv[1] : "";
    if (std::strcmp(prefix, "--list") == 0)
    {
        for (const auto& test : Registry())
        {
            std::printf("%s\n", test.name);
        }
        return 0;
    }

    int run = 0;
    int failedTests = 0;
    for (const auto& test : Registry())
    {
        if (std::strncmp(test.name, prefix, std::strlen(prefix)) != 0)
        {
            continue;
        }

        ++run;
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        try
        {
            test.fn();
        }
        catch (const RequireFailed&)
        {
        }
        catch (const std::exception& ex)
        {
            ReportFailure(__FILE__, __LINE__, ex.what());
        }

        if (g_failuresInCurrentTest == 0)
        {
            std::printf("[  OK  ] %s\n", test.name);
        }
        else
        {
            ++failedTests;
            std::printf("[ FAIL ] %s\n", test.name);
        }
    }

    std::printf("%d test(s) run, %d failed\n", run, failedTests);
    if (run == 0)
    {
        std::fprintf(stderr, "no tests match prefix '%s'\n", prefix);
        return 1;
    }
    return (failedTests == 0) ? 0 : 1;
}
