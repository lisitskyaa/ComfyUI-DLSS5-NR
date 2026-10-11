// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ComfyUI-DLSS5-NR contributors

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nvof_flow.h"

using Microsoft::WRL::ComPtr;
using NGXResult = int;
static constexpr NGXResult NGX_SUCCESS = 1;
static constexpr int NR_FEATURE_ID = 18;
static constexpr unsigned long long APP_ID = 141959980ULL;
static constexpr const char* PROJECT_ID = "53f803cc-a12f-4d69-90d5-19b7599cad19";

struct NGXHandle { unsigned int Id; };

// Minimal ABI-compatible interface used by the NVIDIA NGX parameter object.
struct NGXParameter {
    virtual void Set(const char*, unsigned long long) = 0;
    virtual void Set(const char*, float) = 0;
    virtual void Set(const char*, double) = 0;
    virtual void Set(const char*, unsigned int) = 0;
    virtual void Set(const char*, int) = 0;
    virtual void Set(const char*, ID3D11Resource*) = 0;
    virtual void Set(const char*, ID3D12Resource*) = 0;
    virtual void Set(const char*, void*) = 0;
    virtual NGXResult Get(const char*, unsigned long long*) const = 0;
    virtual NGXResult Get(const char*, float*) const = 0;
    virtual NGXResult Get(const char*, double*) const = 0;
    virtual NGXResult Get(const char*, unsigned int*) const = 0;
    virtual NGXResult Get(const char*, int*) const = 0;
    virtual NGXResult Get(const char*, ID3D11Resource**) const = 0;
    virtual NGXResult Get(const char*, ID3D12Resource**) const = 0;
    virtual NGXResult Get(const char*, void**) const = 0;
    virtual void Reset() = 0;
};

struct NGXPathListInfo {
    wchar_t const* const* Path;
    unsigned int Length;
};
enum NGXLoggingLevel { NGX_LOG_OFF = 0, NGX_LOG_ON = 1, NGX_LOG_VERBOSE = 2 };
using NGXLogCallback = void(__cdecl*)(const char*, NGXLoggingLevel, int);
struct NGXLoggingInfo {
    NGXLoggingLevel LoggingLevel;
    NGXLogCallback Callback;
    void* UserData;
    bool DisableOtherLoggingSinks;
};
struct NGXFeatureCommonInfoInternal;
struct NGXFeatureCommonInfo {
    NGXPathListInfo PathListInfo;
    NGXFeatureCommonInfoInternal* InternalData;
    NGXLoggingInfo LoggingInfo;
};

using InitExtFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using SnippetInitFn = NGXResult(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, const void*, int);
using InitProjectIdFn = NGXResult(__cdecl*)(const char*, int, const char*, const wchar_t*, ID3D12Device*, int, const void*);
using AllocParamsFn = NGXResult(__cdecl*)(NGXParameter**);
using CreateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using EvaluateFeatureFn = NGXResult(__cdecl*)(ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ReleaseFeatureFn = NGXResult(__cdecl*)(NGXHandle*);
using ShutdownFn = NGXResult(__cdecl*)();

using ShimInitFn = NGXResult(__cdecl*)(void*, unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using ShimCreateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, int, NGXParameter*, NGXHandle**);
using ShimEvaluateFn = NGXResult(__cdecl*)(void*, ID3D12GraphicsCommandList*, const NGXHandle*, const NGXParameter*, void*);
using ShimReleaseFn = NGXResult(__cdecl*)(void*, NGXHandle*);

static std::mutex g_mutex;
static std::string g_last_error;
static std::wstring g_runtime_dir;
static int g_gpu_index = 0;
static std::string g_gpu_name = "unknown";
static bool g_initialized = false;

static HMODULE g_core_mod = nullptr;
static HMODULE g_nr_mod = nullptr;
static HMODULE g_shim_mod = nullptr;
static InitExtFn g_core_init_ext = nullptr;
static InitProjectIdFn g_core_init_project = nullptr;
static AllocParamsFn g_alloc_params = nullptr;
static CreateFeatureFn g_core_create = nullptr;
static EvaluateFeatureFn g_core_eval = nullptr;
static ReleaseFeatureFn g_core_release = nullptr;
static ShutdownFn g_core_shutdown = nullptr;
static SnippetInitFn g_nr_init = nullptr;
static CreateFeatureFn g_nr_create = nullptr;
static EvaluateFeatureFn g_nr_eval = nullptr;
static ReleaseFeatureFn g_nr_release = nullptr;
static ShimInitFn g_shim_init = nullptr;
static ShimCreateFn g_shim_create = nullptr;
static ShimEvaluateFn g_shim_eval = nullptr;
static ShimReleaseFn g_shim_release = nullptr;

static ComPtr<IDXGIAdapter1> g_adapter;
static ComPtr<ID3D12Device> g_device;
static ComPtr<ID3D12CommandQueue> g_queue;
static ComPtr<ID3D12CommandAllocator> g_cmd_alloc;
static ComPtr<ID3D12GraphicsCommandList> g_cmd;
static ComPtr<ID3D12Fence> g_fence;
static UINT64 g_fence_value = 0;

static NGXParameter* g_params = nullptr;
static NGXHandle* g_feature = nullptr;
static ComPtr<ID3D12Resource> g_color;
static ComPtr<ID3D12Resource> g_output;
static ComPtr<ID3D12Resource> g_upload;
static ComPtr<ID3D12Resource> g_readback;
// v0.3.0: temporal mode always supplies a full-resolution R16G16_FLOAT
// motion-vector texture. Frame 0 uses zeros; later frames are generated by NVOFA.
static ComPtr<ID3D12Resource> g_mvec;
static ComPtr<ID3D12Resource> g_mvec_upload;
static UINT g_width = 0, g_height = 0, g_row_pitch = 0;
static UINT64 g_total_bytes = 0;
static UINT g_mvec_row_pitch = 0;
static UINT64 g_mvec_total_bytes = 0;
static int g_feature_style = -999;
static int g_feature_preset = -999;
static int g_feature_motion = -1;

static void SetError(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_error = buf;
}

static void CopyError(char* dst, int cap) {
    if (!dst || cap <= 0) return;
    const size_t n = std::min<size_t>(g_last_error.size(), static_cast<size_t>(cap - 1));
    memcpy(dst, g_last_error.data(), n);
    dst[n] = '\0';
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t c = a.back();
    if (c == L'\\' || c == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned long long FileTimeKey(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

static HMODULE LoadCoreNGX(const std::wstring& runtime) {
    // 1) Explicit local override. This is also the quickest workaround if
    // DriverStore auto-discovery ever misses a vendor-specific INF name.
    const std::wstring local = Join(runtime, L"_nvngx.dll");
    if (FileExists(local)) {
        if (HMODULE m = LoadLibraryW(local.c_str())) return m;
    }

    // 2) Normal loader search (works on systems where NVIDIA exposes it).
    if (HMODULE m = LoadLibraryW(L"_nvngx.dll")) return m;

    // 3) NVIDIA ships NGX core inside the active display-driver package in
    // DriverStore. The INF prefix is NOT always nv_dispi: depending on OEM,
    // notebook/desktop package and driver generation it can be nvddi, nvaci,
    // nvhmui, etc. Scan every NVIDIA-looking *.inf_* package instead.
    wchar_t windows_dir[MAX_PATH] = {};
    UINT windows_len = GetWindowsDirectoryW(windows_dir, MAX_PATH);
    if (windows_len == 0 || windows_len >= MAX_PATH) return nullptr;
    const std::wstring repo = std::wstring(windows_dir) + L"\\System32\\DriverStore\\FileRepository";
    const std::wstring pat = repo + L"\\nv*.inf_*";

    struct Candidate {
        std::wstring path;
        unsigned long long stamp;
    };
    std::vector<Candidate> candidates;

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

            std::wstring candidate = repo + L"\\" + fd.cFileName + L"\\_nvngx.dll";
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad) &&
                !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                candidates.push_back({candidate, FileTimeKey(fad.ftLastWriteTime)});
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Prefer the newest package. DriverStore often retains older drivers after
    // updates, and loading a stale NGX core is worse than not finding one.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.stamp > b.stamp; });

    for (const Candidate& c : candidates) {
        if (HMODULE m = LoadLibraryW(c.path.c_str())) return m;
    }

    return nullptr;
}

static ComPtr<ID3D12Device> CreateDevice(int nvidia_index) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    int seen = 0;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId != 0x10DE) continue;
        if (seen++ != nvidia_index) continue;
        char gpu_utf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu_utf8, static_cast<int>(sizeof(gpu_utf8)), nullptr, nullptr);
        if (gpu_utf8[0]) g_gpu_name = gpu_utf8;
        ComPtr<ID3D12Device> d;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d)))) {
            g_adapter = adapter;
            return d;
        }
        return nullptr;
    }
    return nullptr;
}

static bool SetupD3D12() {
    g_device = CreateDevice(g_gpu_index);
    if (!g_device) { SetError("Could not create a D3D12 device for NVIDIA GPU index %d", g_gpu_index); return false; }

    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&q, IID_PPV_ARGS(&g_queue)))) { SetError("CreateCommandQueue failed"); return false; }
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_cmd_alloc)))) { SetError("CreateCommandAllocator failed"); return false; }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_cmd_alloc.Get(), nullptr, IID_PPV_ARGS(&g_cmd)))) { SetError("CreateCommandList failed"); return false; }
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) { SetError("CreateFence failed"); return false; }
    return true;
}

static bool ExecuteAndWait() {
    HRESULT hr = g_cmd->Close();
    if (FAILED(hr)) { SetError("CommandList::Close failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    ID3D12CommandList* lists[] = { g_cmd.Get() };
    g_queue->ExecuteCommandLists(1, lists);
    ++g_fence_value;
    hr = g_queue->Signal(g_fence.Get(), g_fence_value);
    if (FAILED(hr)) { SetError("Queue::Signal failed (0x%08X)", static_cast<unsigned>(hr)); return false; }
    if (g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ev) { SetError("CreateEvent failed"); return false; }
        g_fence->SetEventOnCompletion(g_fence_value, ev);
        DWORD w = WaitForSingleObject(ev, 30000);
        CloseHandle(ev);
        if (w != WAIT_OBJECT_0) { SetError("Timed out waiting for DLSS5 NR GPU work"); return false; }
    }
    g_cmd_alloc->Reset();
    g_cmd->Reset(g_cmd_alloc.Get(), nullptr);
    return true;
}

static void WaitQueueIdle() {
    if (!g_queue || !g_fence) return;
    ++g_fence_value;
    if (SUCCEEDED(g_queue->Signal(g_fence.Get(), g_fence_value)) && g_fence->GetCompletedValue() < g_fence_value) {
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            g_fence->SetEventOnCompletion(g_fence_value, ev);
            WaitForSingleObject(ev, 30000);
            CloseHandle(ev);
        }
    }
}

static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

static ComPtr<ID3D12Resource> CreateTexture(
    UINT w, UINT h, DXGI_FORMAT format,
    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static ComPtr<ID3D12Resource> CreateLinearBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    ComPtr<ID3D12Resource> r;
    if (FAILED(g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

static uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, sizeof(x));
    uint32_t s = (x >> 16) & 0x8000u;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffffu;
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(s);
        m = (m | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>(s | (m >> 13));
    }
    if (e >= 31) return static_cast<uint16_t>(s | 0x7c00u);
    return static_cast<uint16_t>(s | (static_cast<uint32_t>(e) << 10) | (m >> 13));
}

static float HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (m == 0) x = s << 31;
        else {
            e = 1;
            while (!(m & 0x400)) { m <<= 1; --e; }
            m &= 0x3ff;
            x = (s << 31) | ((e + 112) << 23) | (m << 13);
        }
    } else if (e == 0x1f) x = (s << 31) | 0x7f800000u | (m << 13);
    else x = (s << 31) | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, sizeof(f)); return f;
}

static bool UploadMotionVectorTexture(const NvofFlowFrame* flow, bool execute_now) {
    if (!g_mvec || !g_mvec_upload || g_width == 0 || g_height == 0) {
        SetError("Motion-vector resources are not allocated");
        return false;
    }

    void* mapped = nullptr;
    HRESULT hr = g_mvec_upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        SetError("Motion-vector upload Map failed: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    memset(mapped, 0, static_cast<size_t>(g_mvec_total_bytes));

    if (flow && flow->has_flow) {
        if (flow->width == 0 || flow->height == 0 ||
            flow->xy.size() < static_cast<size_t>(flow->width) * flow->height * 2) {
            g_mvec_upload->Unmap(0, nullptr);
            SetError("NVIDIA Optical Flow returned an invalid flow field");
            return false;
        }

        // NVOF returns S10.5 current->previous displacement in pixels. DLSS's
        // contract stores normalized UV and multiplies by MVecScale=(W,H), so:
        //   stored = fixed / 32 / axis_size
        // Use nearest reconstruction from the NVOF grid to avoid inventing
        // vectors across disocclusion boundaries.
        auto* dst_base = static_cast<uint8_t*>(mapped);
        for (UINT y = 0; y < g_height; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(dst_base + static_cast<size_t>(y) * g_mvec_row_pitch);
            const UINT cy = std::min<UINT>(
                static_cast<UINT>((static_cast<uint64_t>(y) * flow->height) / g_height),
                flow->height - 1);
            for (UINT x = 0; x < g_width; ++x) {
                const UINT cx = std::min<UINT>(
                    static_cast<UINT>((static_cast<uint64_t>(x) * flow->width) / g_width),
                    flow->width - 1);
                const size_t i = (static_cast<size_t>(cy) * flow->width + cx) * 2;
                const float uvx = static_cast<float>(flow->xy[i + 0]) * (1.0f / 32.0f) / static_cast<float>(g_width);
                const float uvy = static_cast<float>(flow->xy[i + 1]) * (1.0f / 32.0f) / static_cast<float>(g_height);
                row[x * 2 + 0] = FloatToHalf(uvx);
                row[x * 2 + 1] = FloatToHalf(uvy);
            }
        }
    }
    g_mvec_upload->Unmap(0, nullptr);

    auto to_copy = Barrier(
        g_mvec.Get(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    g_cmd->ResourceBarrier(1, &to_copy);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_mvec.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_mvec_upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_FLOAT;
    src.PlacedFootprint.Footprint.Width = g_width;
    src.PlacedFootprint.Footprint.Height = g_height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = g_mvec_row_pitch;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    auto to_read = Barrier(
        g_mvec.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &to_read);

    return !execute_now || ExecuteAndWait();
}

static void ReleaseFeatureAndResources() {
    WaitQueueIdle();
    if (g_feature) {
        if (g_nr_release && g_shim_release) g_shim_release(reinterpret_cast<void*>(g_nr_release), g_feature);
        else if (g_core_release) g_core_release(g_feature);
        g_feature = nullptr;
    }

    // NGX parameter objects do not necessarily AddRef resources stored in them.
    if (g_params) {
        g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
    }

    g_color.Reset(); g_output.Reset(); g_upload.Reset(); g_readback.Reset();
    g_mvec.Reset(); g_mvec_upload.Reset();
    g_width = g_height = g_row_pitch = 0;
    g_total_bytes = 0;
    g_mvec_row_pitch = 0;
    g_mvec_total_bytes = 0;
    g_feature_style = -999; g_feature_preset = -999; g_feature_motion = -1;
}

static bool AllocateFrameResources(UINT w, UINT h, bool use_motion_vectors) {
    g_color = CreateTexture(
        w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    g_output = CreateTexture(
        w, h, DXGI_FORMAT_R16G16B16A16_FLOAT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!g_color || !g_output) { SetError("Failed to create RGBA16F D3D12 textures"); return false; }

    g_row_pitch = (w * 8u + 255u) & ~255u;
    g_total_bytes = static_cast<UINT64>(g_row_pitch) * h;
    g_upload = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    g_readback = CreateLinearBuffer(g_total_bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!g_upload || !g_readback) { SetError("Failed to create D3D12 upload/readback buffers"); return false; }

    g_width = w; g_height = h;

    if (use_motion_vectors) {
        g_mvec = CreateTexture(
            w, h, DXGI_FORMAT_R16G16_FLOAT,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_FLAG_NONE);
        if (!g_mvec) { SetError("Failed to create R16G16_FLOAT motion-vector texture"); return false; }

        g_mvec_row_pitch = (w * 4u + 255u) & ~255u;
        g_mvec_total_bytes = static_cast<UINT64>(g_mvec_row_pitch) * h;
        g_mvec_upload = CreateLinearBuffer(
            g_mvec_total_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!g_mvec_upload) { SetError("Failed to create motion-vector upload buffer"); return false; }

        // Feature creation always starts with a defined zero-MV resource. The
        // first temporal frame also uses this; NVOF has no previous frame yet.
        if (!UploadMotionVectorTexture(nullptr, true)) return false;
    }

    return true;
}

static void SetCommonParams(
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, bool use_motion_vectors) {
    g_params->Set("DLSSNR.Width", g_width);
    g_params->Set("DLSSNR.Height", g_height);
    g_params->Set("DLSSNR.Enabled", 1);
    g_params->Set("DLSSNR.Reset", reset);
    g_params->Set("DLSSNR.Style", style);
    g_params->Set("DLSSNR.Hint.Render.Preset", preset);
    g_params->Set("DLSSNR.Intensity", intensity);
    g_params->Set("DLSSNR.LocalToneStrength", tone);
    g_params->Set("DLSSNR.LocalStructureStrength", structure);
    g_params->Set("DLSSNR.SkinStructureStrength", skin);
    g_params->Set("DLSSNR.UseAutoMask", automask);
    g_params->Set("DLSSNR.UICorrection", 0);
    g_params->Set("DLSSNR.DepthInverted", 1);
    g_params->Set("DLSSNR.ScalingRatio", 1.0f);
    g_params->Set("DLSSNR.Color", g_color.Get());
    g_params->Set("DLSSNR.Output", g_output.Get());
    g_params->Set("DLSSNR.Backbuffer", g_output.Get());
    g_params->Set("DLSSNR.ColorSubrectBaseX", 0);
    g_params->Set("DLSSNR.ColorSubrectBaseY", 0);
    g_params->Set("DLSSNR.ColorSubrectWidth", g_width);
    g_params->Set("DLSSNR.ColorSubrectHeight", g_height);
    g_params->Set("DLSSNR.OutputSubrectBaseX", 0);
    g_params->Set("DLSSNR.OutputSubrectBaseY", 0);
    g_params->Set("DLSSNR.OutputSubrectWidth", g_width);
    g_params->Set("DLSSNR.OutputSubrectHeight", g_height);

    if (use_motion_vectors && g_mvec) {
        g_params->Set("DLSSNR.MVec", g_mvec.Get());
        // g_mvec stores normalized UV. Multiplying by dimensions reconstructs
        // the original pixel displacement from NVOF exactly.
        g_params->Set("DLSSNR.MVecScaleX", static_cast<float>(g_width));
        g_params->Set("DLSSNR.MVecScaleY", static_cast<float>(g_height));
        g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
        g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
        g_params->Set("DLSSNR.MVecSubrectWidth", g_width);
        g_params->Set("DLSSNR.MVecSubrectHeight", g_height);
    } else {
        g_params->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
        g_params->Set("DLSSNR.MVecScaleX", 1.0f);
        g_params->Set("DLSSNR.MVecScaleY", 1.0f);
        g_params->Set("DLSSNR.MVecSubrectBaseX", 0);
        g_params->Set("DLSSNR.MVecSubrectBaseY", 0);
        g_params->Set("DLSSNR.MVecSubrectWidth", 0);
        g_params->Set("DLSSNR.MVecSubrectHeight", 0);
    }
}

static bool EnsureFeature(
    UINT w, UINT h, int style, int preset, float intensity, float tone, float structure,
    float skin, int automask, bool use_motion_vectors) {
    const int motion_key = use_motion_vectors ? 1 : 0;
    const bool rebuild = !g_feature || w != g_width || h != g_height ||
        style != g_feature_style || preset != g_feature_preset ||
        motion_key != g_feature_motion;
    if (!rebuild) {
        SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 0, use_motion_vectors);
        return true;
    }

    ReleaseFeatureAndResources();
    if (!AllocateFrameResources(w, h, use_motion_vectors)) return false;
    SetCommonParams(style, preset, intensity, tone, structure, skin, automask, 1, use_motion_vectors);

    NGXResult r;
    if (g_nr_create && g_shim_create)
        r = g_shim_create(reinterpret_cast<void*>(g_nr_create), g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    else
        r = g_core_create(g_cmd.Get(), NR_FEATURE_ID, g_params, &g_feature);
    if (r != NGX_SUCCESS || !g_feature) {
        SetError("CreateFeature(18) failed: 0x%08X. Check GPU support, driver, nvngx_dlssnr.dll, and caller shim.", static_cast<unsigned>(r));
        return false;
    }
    g_feature_style = style;
    g_feature_preset = preset;
    g_feature_motion = motion_key;
    return true;
}

static bool LoadNGX() {
    g_core_mod = LoadCoreNGX(g_runtime_dir);
    if (!g_core_mod) {
        SetError("Could not load NVIDIA NGX core _nvngx.dll. Tried runtime\\_nvngx.dll, normal DLL search, and NVIDIA DriverStore packages matching nv*.inf_*. You can copy the _nvngx.dll from your active NVIDIA DriverStore folder into runtime\\_nvngx.dll as an explicit override.");
        return false;
    }

    const std::wstring nr_path = Join(g_runtime_dir, L"nvngx_dlssnr.dll");
    if (!FileExists(nr_path)) { SetError("nvngx_dlssnr.dll not found in runtime folder"); return false; }
    g_nr_mod = LoadLibraryW(nr_path.c_str());
    if (!g_nr_mod) { SetError("LoadLibrary(nvngx_dlssnr.dll) failed: Win32 %lu", GetLastError()); return false; }

    std::wstring shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll_comfy.dll");
    if (!FileExists(shim_path)) {
        // Backward-compatible fallback for older builds.
        shim_path = Join(Join(g_runtime_dir, L"caller"), L"nvngx.dll");
    }
    if (!FileExists(shim_path)) { SetError("caller shim not found (expected caller\\nvngx.dll_comfy.dll)"); return false; }
    g_shim_mod = LoadLibraryW(shim_path.c_str());
    if (!g_shim_mod) { SetError("LoadLibrary(caller shim) failed: Win32 %lu", GetLastError()); return false; }

    g_core_init_ext = reinterpret_cast<InitExtFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_core_init_project = reinterpret_cast<InitProjectIdFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Init_ProjectID"));
    g_alloc_params = reinterpret_cast<AllocParamsFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_AllocateParameters"));
    g_core_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_core_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_core_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));
    g_core_shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(g_core_mod, "NVSDK_NGX_D3D12_Shutdown"));

    g_nr_init = reinterpret_cast<SnippetInitFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_Init_Ext"));
    g_nr_create = reinterpret_cast<CreateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_CreateFeature"));
    g_nr_eval = reinterpret_cast<EvaluateFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g_nr_release = reinterpret_cast<ReleaseFeatureFn>(GetProcAddress(g_nr_mod, "NVSDK_NGX_D3D12_ReleaseFeature"));

    g_shim_init = reinterpret_cast<ShimInitFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallInit"));
    g_shim_create = reinterpret_cast<ShimCreateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallCreate"));
    g_shim_eval = reinterpret_cast<ShimEvaluateFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallEvaluate"));
    g_shim_release = reinterpret_cast<ShimReleaseFn>(GetProcAddress(g_shim_mod, "DLSSNR_CallRelease"));

    if (!g_core_init_ext || !g_alloc_params || !g_core_create || !g_core_eval || !g_core_release || !g_core_shutdown) {
        SetError("Required NGX core exports are missing"); return false;
    }
    if (!g_nr_init || !g_nr_create || !g_nr_eval || !g_nr_release) {
        SetError("Required DLSSNR exports are missing from nvngx_dlssnr.dll"); return false;
    }
    if (!g_shim_init || !g_shim_create || !g_shim_eval || !g_shim_release) {
        SetError("Required caller shim exports are missing"); return false;
    }
    return true;
}

static bool InitNGXSession() {
    const wchar_t* paths[1] = { g_runtime_dir.c_str() };
    NGXPathListInfo pli{ paths, 1 };
    NGXFeatureCommonInfo fci{};
    fci.PathListInfo = pli;
    fci.LoggingInfo.LoggingLevel = NGX_LOG_OFF;

    bool core_ok = false;
    if (g_core_init_project) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_project(PROJECT_ID, 0, "0.3.1", g_runtime_dir.c_str(), g_device.Get(), ver, nullptr);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) {
        for (int ver = 0x13; ver <= 0x20 && !core_ok; ++ver) {
            NGXResult r = g_core_init_ext(APP_ID, g_runtime_dir.c_str(), g_device.Get(), ver, &fci);
            core_ok = (r == NGX_SUCCESS);
        }
    }
    if (!core_ok) { SetError("NGX core initialization failed for API versions 0x13..0x20"); return false; }

    NGXResult sr = g_shim_init(reinterpret_cast<void*>(g_nr_init), APP_ID, g_runtime_dir.c_str(), g_device.Get(), 0x15, &fci);
    if (sr != NGX_SUCCESS) {
        wchar_t shim_self[MAX_PATH] = L"<unknown>";
        GetModuleFileNameW(g_shim_mod, shim_self, MAX_PATH);
        char shim_utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, shim_self, -1, shim_utf8, static_cast<int>(sizeof(shim_utf8)), nullptr, nullptr);
        SetError("DLSSNR snippet Init_Ext via caller shim failed: 0x%08X; loaded shim=%s", static_cast<unsigned>(sr), shim_utf8);
        return false;
    }

    NGXResult ar = g_alloc_params(&g_params);
    if (ar != NGX_SUCCESS || !g_params) {
        SetError("NVSDK_NGX_D3D12_AllocateParameters failed: 0x%08X", static_cast<unsigned>(ar));
        return false;
    }
    return true;
}

static void ShutdownUnlocked() {
    ReleaseFeatureAndResources();
    NvofShutdown();
    if (g_core_shutdown) g_core_shutdown();
    g_params = nullptr;
    g_device.Reset(); g_adapter.Reset(); g_queue.Reset(); g_cmd_alloc.Reset(); g_cmd.Reset(); g_fence.Reset();
    if (g_shim_mod) FreeLibrary(g_shim_mod);
    if (g_nr_mod) FreeLibrary(g_nr_mod);
    if (g_core_mod) FreeLibrary(g_core_mod);
    g_shim_mod = g_nr_mod = g_core_mod = nullptr;

    g_core_init_ext = nullptr;
    g_core_init_project = nullptr;
    g_alloc_params = nullptr;
    g_core_create = nullptr;
    g_core_eval = nullptr;
    g_core_release = nullptr;
    g_core_shutdown = nullptr;
    g_nr_init = nullptr;
    g_nr_create = nullptr;
    g_nr_eval = nullptr;
    g_nr_release = nullptr;
    g_shim_init = nullptr;
    g_shim_create = nullptr;
    g_shim_eval = nullptr;
    g_shim_release = nullptr;

    g_initialized = false;
}

extern "C" {

__declspec(dllexport) const char* __cdecl dlss5nr_version() {
    return "0.3.1-cpu-staging-nvof";
}

__declspec(dllexport) const char* __cdecl dlss5nr_gpu_name() {
    return g_gpu_name.c_str();
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_available() {
    return NvofDriverApiAvailable() ? 1 : 0;
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_grid() {
    return static_cast<int>(NvofGridSize());
}

__declspec(dllexport) int __cdecl dlss5nr_nvof_perf() {
    return static_cast<int>(NvofPerfLevel());
}

__declspec(dllexport) const char* __cdecl dlss5nr_nvof_info() {
    return NvofDiagnostics();
}

__declspec(dllexport) int __cdecl dlss5nr_init(int gpu_index, const wchar_t* runtime_dir, char* err, int err_cap) {
    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (g_initialized) { CopyError(err, err_cap); return 1; }
    if (!runtime_dir || !*runtime_dir) { SetError("runtime_dir is empty"); CopyError(err, err_cap); return 0; }

    g_gpu_index = gpu_index;
    g_runtime_dir = runtime_dir;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)co; // RPC_E_CHANGED_MODE is harmless for this use.

    if (!SetupD3D12() || !LoadNGX() || !InitNGXSession()) {
        ShutdownUnlocked();
        CopyError(err, err_cap);
        return 0;
    }
    g_initialized = true;
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) int __cdecl dlss5nr_process(
    const float* rgb_in, float* rgb_out, int width, int height,
    int style, int preset, float intensity, float tone, float structure, float skin,
    int automask, int reset, int temporal, char* err, int err_cap) {

    std::lock_guard<std::mutex> guard(g_mutex);
    g_last_error.clear();
    if (!g_initialized) { SetError("DLSS5 NR bridge is not initialized"); CopyError(err, err_cap); return 0; }
    if (!rgb_in || !rgb_out || width <= 0 || height <= 0) { SetError("Invalid image buffer/dimensions"); CopyError(err, err_cap); return 0; }
    if (width > 16384 || height > 16384) { SetError("Image dimensions are unreasonably large"); CopyError(err, err_cap); return 0; }

    const bool use_motion_vectors = temporal != 0;
    if (!use_motion_vectors) {
        // Still-image mode owns no temporal history or OFA resources. This also
        // releases OFA VRAM when a workflow switches from temporal back to still.
        NvofReleaseSession();
    }

    if (!EnsureFeature(static_cast<UINT>(width), static_cast<UINT>(height), style, preset, intensity, tone, structure, skin, automask, use_motion_vectors)) {
        CopyError(err, err_cap); return 0;
    }

    if (use_motion_vectors) {
        NvofFlowFrame flow;
        std::string of_error;
        if (!NvofPrepareFrame(g_adapter.Get(), rgb_in, static_cast<UINT>(width), static_cast<UINT>(height), reset != 0, flow, of_error)) {
            SetError("%s", of_error.c_str());
            CopyError(err, err_cap);
            return 0;
        }
        // First frame: no previous image exists, so this deliberately writes zero
        // MVs. Later frames upload NVOFA current->previous optical flow.
        if (!UploadMotionVectorTexture(flow.has_flow ? &flow : nullptr, false)) {
            CopyError(err, err_cap);
            return 0;
        }
    }

    SetCommonParams(style, preset, intensity, tone, structure, skin, automask, reset ? 1 : 0, use_motion_vectors);

    void* mapped = nullptr;
    HRESULT hr = g_upload->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) { SetError("Upload buffer Map failed: 0x%08X", static_cast<unsigned>(hr)); CopyError(err, err_cap); return 0; }
    memset(mapped, 0, static_cast<size_t>(g_total_bytes));
    auto* dst_base = static_cast<uint8_t*>(mapped);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(dst_base + static_cast<size_t>(y) * g_row_pitch);
        const float* src = rgb_in + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = FloatToHalf(std::clamp(src[x * 3 + 0], 0.0f, 1.0f));
            row[x * 4 + 1] = FloatToHalf(std::clamp(src[x * 3 + 1], 0.0f, 1.0f));
            row[x * 4 + 2] = FloatToHalf(std::clamp(src[x * 3 + 2], 0.0f, 1.0f));
            row[x * 4 + 3] = FloatToHalf(1.0f);
        }
    }
    g_upload->Unmap(0, nullptr);

    auto b1 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_cmd->ResourceBarrier(1, &b1);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g_upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    src.PlacedFootprint.Footprint.Width = width; src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
    g_cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    auto b2 = Barrier(g_color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_cmd->ResourceBarrier(1, &b2);

    // The runtime blends against Backbuffer when intensity < 1. Seed it with
    // this image rather than undefined memory or the previous image's output.
    auto backbuffer_to_copy = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    g_cmd->ResourceBarrier(1, &backbuffer_to_copy);
    D3D12_TEXTURE_COPY_LOCATION backbuffer{};
    backbuffer.pResource = g_output.Get();
    backbuffer.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmd->CopyTextureRegion(&backbuffer, 0, 0, 0, &src, nullptr);
    auto backbuffer_to_uav = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cmd->ResourceBarrier(1, &backbuffer_to_uav);

    NGXResult er = g_shim_eval(reinterpret_cast<void*>(g_nr_eval), g_cmd.Get(), g_feature, g_params, nullptr);
    if (er != NGX_SUCCESS) {
        SetError("DLSSNR EvaluateFeature failed: 0x%08X", static_cast<unsigned>(er));
        // Reset command list to a clean state before returning.
        ExecuteAndWait();
        CopyError(err, err_cap); return 0;
    }

    auto b3 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cmd->ResourceBarrier(1, &b3);
    D3D12_TEXTURE_COPY_LOCATION rd{};
    rd.pResource = g_readback.Get(); rd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rd.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.PlacedFootprint.Footprint.Width = width; rd.PlacedFootprint.Footprint.Height = height;
    rd.PlacedFootprint.Footprint.Depth = 1; rd.PlacedFootprint.Footprint.RowPitch = g_row_pitch;
    D3D12_TEXTURE_COPY_LOCATION rs{};
    rs.pResource = g_output.Get(); rs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_cmd->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
    auto b4 = Barrier(g_output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_cmd->ResourceBarrier(1, &b4);

    if (!ExecuteAndWait()) { CopyError(err, err_cap); return 0; }

    void* rmap = nullptr;
    hr = g_readback->Map(0, nullptr, &rmap);
    if (FAILED(hr) || !rmap) { SetError("Readback Map failed: 0x%08X", static_cast<unsigned>(hr)); CopyError(err, err_cap); return 0; }
    const auto* base = static_cast<const uint8_t*>(rmap);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * g_row_pitch);
        float* dstf = rgb_out + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            // Return the resource channels exactly as stored. Some stock/reference
            // DLSSNR builds have been observed to produce B,G,R,A while patched
            // Ada builds may produce R,G,B,A. Python selects/auto-detects the
            // correct interpretation instead of hard-coding a swap here.
            dstf[x * 3 + 0] = std::clamp(HalfToFloat(row[x * 4 + 0]), 0.0f, 1.0f);
            dstf[x * 3 + 1] = std::clamp(HalfToFloat(row[x * 4 + 1]), 0.0f, 1.0f);
            dstf[x * 3 + 2] = std::clamp(HalfToFloat(row[x * 4 + 2]), 0.0f, 1.0f);
        }
    }
    g_readback->Unmap(0, nullptr);
    CopyError(err, err_cap);
    return 1;
}

__declspec(dllexport) void __cdecl dlss5nr_shutdown() {
    std::lock_guard<std::mutex> guard(g_mutex);
    ShutdownUnlocked();
}

}
