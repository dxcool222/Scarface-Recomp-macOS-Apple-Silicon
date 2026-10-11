// d3d9.cpp - Direct3D 9, for a game that renders through shaders.
//
// The kit's other Direct3D module (d3d.cpp) models version 2: a fixed-function
// recorder reached by QueryInterface on a DirectDraw object. Version 9 shares
// none of that. It is its own DLL with its own factory function, its own
// object model, and a device whose drawing is programmable, so it lives here.
//
// What works today: the factory object, everything a game asks it before it
// commits to a device (adapter count, identifier, display mode, device type
// and format checks, caps) and CreateDevice itself. The device's vtable is
// complete and correctly ordered - a guest calls these by slot index, so the
// order is the interface - but only the handful of methods below do anything;
// every other slot reports itself once and returns D3D_OK. That turns what
// was a crash into a list of exactly which device methods this game uses,
// which is what the drawing work needs next.
//
// Nothing here rasterizes. No pixels are produced by this file yet.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"
#include "host_api.h"
#include "d3d9_pipeline.h"
#include "d3d9_raster.h"
#include "d3d9_shader.h"
#include "host_d9.h"
#include "../platform/os.h"
#include <unordered_map>
#include <map>

#include <string.h>
#include <time.h>
#include <iterator>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <cstdlib>

#define IID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}

// {81BDCBCA-64D4-426d-AE8D-AD0147F4275C} and {D0223B96-BF7A-43fd-92BD-A43B0D82B9EB}
static const uint8_t IID_IDirect3D9_[16] =
    IID_BYTES(0x81bdcbca, 0x64d4, 0x426d, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c);
static const uint8_t IID_IDirect3DDevice9_[16] =
    IID_BYTES(0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);

#define MAKE_D3DHRESULT(code) (0x88760000u | (uint32_t)(code))
static const uint32_t D3D_OK9 = 0u;
static const uint32_t D3DERR_NOTAVAILABLE = MAKE_D3DHRESULT(2154);
static const uint32_t D3DERR_INVALIDCALL = MAKE_D3DHRESULT(2156);
static const uint32_t D3DERR_DEVICELOST = MAKE_D3DHRESULT(2152);
static const uint32_t D3DERR_DEVICENOTRESET = MAKE_D3DHRESULT(2153);

static std::atomic<bool> g_d3d9_focused{true};
static std::atomic<uint64_t> g_d3d9_focus_loss_epoch{0};
static bool d3d_rs_trace() {
    static const bool enabled = recomp_env("D3D_RS_TRACE") != nullptr;
    return enabled;
}
static bool d3d_color_trace() {
    static const bool enabled = recomp_env("D3D_COLOR_TRACE") != nullptr;
    return enabled;
}
// The device CreateDevice most recently returned. Scarface keeps one.
static std::atomic<uint32_t> g_render_device_id{0};
static std::atomic<bool> g_render_device_live{false};

static uint32_t fullscreen_cooperative_status(D9Pipeline &pl) {
    const uint64_t focus_loss_epoch = g_d3d9_focus_loss_epoch.load();
    if (!pl.windowed && pl.focus_loss_seen != focus_loss_epoch) {
        pl.focus_loss_seen = focus_loss_epoch;
        pl.reset_needed = true;
    }
    if (pl.windowed)
        return D3D_OK9;
    if (!g_d3d9_focused.load())
        return D3DERR_DEVICELOST;
    if (pl.reset_needed)
        return D3DERR_DEVICENOTRESET;
    return D3D_OK9;
}

bool d3d9_render_suspended() {
    if (!g_render_device_live.load())
        return false;
    return fullscreen_cooperative_status(d9_pipeline(g_render_device_id.load())) != D3D_OK9;
}

void d3d9_notify_focus(bool focused) {
    const bool was_focused = g_d3d9_focused.exchange(focused);
    if (was_focused && !focused)
        g_d3d9_focus_loss_epoch.fetch_add(1);
}

// Defined with the resources further down.
struct ComObj;
static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static bool gpu_on();
static HostD9Target gpu_target(ComObj *dev);
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]);
static void gpu_target_drawn(ComObj *dev, bool color, bool depth);
static void gpu_sync(ComObj *o);

// One scratch block for structures a method fills in for the guest.
static uint32_t g_scratch = 0, g_scratch_size = 0;
static uint32_t scratch9(uint32_t n) {
    if (g_scratch_size < n) {
        if (g_scratch)
            heap_free(g_scratch);
        g_scratch = heap_alloc(n, true, 16);
        g_scratch_size = g_scratch ? n : 0;
    }
    if (g_scratch)
        memset(gm_ptr(g_scratch), 0, g_scratch_size);
    return g_scratch;
}

static ComObj *this_d3d9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9) ? o : nullptr;
}
static ComObj *this_device9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9DEVICE) ? o : nullptr;
}

static bool d9_vertex_trace() {
    static const bool enabled = std::getenv("RECOMP_D3D_VERTEX_TRACE") != nullptr;
    return enabled;
}

static void d9_trace_declaration(const ComObj *d) {
    if (!d || d->kind != K_D3D9DECL)
        return;
    for (size_t i = 0; i + 8 <= d->blob.size(); i += 8) {
        const uint8_t *e = d->blob.data() + i;
        uint16_t stream = (uint16_t)(e[0] | (e[1] << 8));
        uint16_t offset = (uint16_t)(e[2] | (e[3] << 8));
        LOGW("d3d9 vertex trace: decl=%u element=%zu stream=%u offset=%u type=%u method=%u usage=%u index=%u raw=%02x%02x%02x%02x%02x%02x%02x%02x",
             d->id, i / 8, stream, offset, e[4], e[5], e[6], e[7], e[0], e[1], e[2], e[3],
             e[4], e[5], e[6], e[7]);
        if (stream == 0xff)
            break;
    }
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------
// One adapter, the host window's display. A game that enumerates further gets
// an honest "there is only one".
void D9_RegisterSoftwareDevice(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
void D9_GetAdapterCount(X86 *c) {
    set_eax(c, 1);
}

// D3DADAPTER_IDENTIFIER9: two 512-byte strings, a 32-byte device name, the
// driver version, four ids, the identifier GUID and the WHQL level.
void D9_GetAdapterIdentifier(X86 *c) {
    uint32_t out = arg(c, 3);
    if (!out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(out), 0, 1100);
    // Games of this era pick their feature set by vendor and device id, and
    // turn off whatever they do not recognise (Most Wanted offers shadows
    // only on NVIDIA and ATI). The renderer covers what a GeForce 7800 GTX
    // offered, hardware shadow maps included, so that is the card it names.
    gm_put_str(out, "nv4_disp.dll", 512);
    gm_put_str(out + 512, "NVIDIA GeForce 7800 GTX", 512);
    gm_put_str(out + 1024, "\\\\.\\DISPLAY1", 32);
    // Driver[512], Description[512], DeviceName[32], then:
    wr32(out + 1056, 0x000a249b); // DriverVersion 6.14.10.9371 (low, high)
    wr32(out + 1060, 0x0006000e);
    wr32(out + 1064, 0x10de); // VendorId
    wr32(out + 1068, 0x0091); // DeviceId
    wr32(out + 1076, 0xa1);   // Revision
    wr32(out + 1096, 1);      // WHQLLevel: certified
    com_ret(c, D3D_OK9);
}

void D9_GetAdapterModeCount(X86 *c) {
    set_eax(c, 1);
}

// D3DDISPLAYMODE: width, height, refresh rate, format. X8R8G8B8 is 22.
static void put_display_mode(uint32_t p) {
    if (!p)
        return;
    wr32(p + 0, 1280);
    wr32(p + 4, 960);
    wr32(p + 8, 60);
    wr32(p + 12, 22);
}
void D9_EnumAdapterModes(X86 *c) {
    put_display_mode(arg(c, 4));
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterDisplayMode(X86 *c) {
    put_display_mode(arg(c, 2));
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceType(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormat(X86 *c) {
    com_ret(c, D3D_OK9);
}
// D3DMULTISAMPLE_TYPE and quality as a sample count: 2x and 4x, which every
// Apple GPU renders. NONMASKABLE (1) offers them as quality levels 0 and 1.
static uint32_t multisample_count(uint32_t type, uint32_t quality) {
    if (type == 1)
        return quality == 0 ? 2 : 4;
    if (type >= 2)
        return type >= 4 ? 4 : 2;
    return 0;
}
// (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
// pQualityLevels)
void D9_CheckDeviceMultiSampleType(X86 *c) {
    uint32_t type = arg(c, 5), quality = arg(c, 6);
    uint32_t levels = type == 0 ? 1 : type == 1 ? 2 : (type == 2 || type == 4) ? 1 : 0;
    if (!levels) {
        com_ret(c, 0x8876086au); // D3DERR_NOTAVAILABLE
        return;
    }
    if (quality && gm_valid(quality, 4))
        wr32(quality, levels);
    com_ret(c, D3D_OK9);
}
void D9_CheckDepthStencilMatch(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormatConversion(X86 *c) {
    com_ret(c, D3D_OK9);
}

// D3DCAPS9 (304 bytes), as a GeForce 7800 GTX reports it for what the
// renderer implements: shader model 3.0, trilinear and 16x anisotropic
// filtering, the usual blend, compare and stencil sets, non-power-of-two
// textures, four render targets.
static void put_caps9(uint32_t p) {
    if (!p)
        return;
    memset(gm_ptr(p), 0, 304);
    auto f = [&](uint32_t off, float v) { wrf32(p + off, v); };
    wr32(p + 0, 1);            // DeviceType: D3DDEVTYPE_HAL
    wr32(p + 8, 0x00020000u);  // Caps: READ_SCANLINE
    wr32(p + 12, 0x00020000u); // Caps2: FULLSCREENGAMMA
    wr32(
        p + 16,
        0x000003a0u); // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM, LINEAR_TO_SRGB
    wr32(p + 20, 0x8000000fu); // PresentationIntervals: IMMEDIATE, ONE..FOUR
    wr32(p + 28, 0x001bbef0u); // DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, HWRASTERIZATION, ...
    wr32(
        p + 32,
        0x0003ccf2u); // PrimitiveMiscCaps: masks, cull modes, blend op, separate alpha, independent write masks
    wr32(
        p + 36,
        0x0f732191u); // RasterCaps: dither, zbias, fog, anisotropy, scissor, slope-scale depth bias
    wr32(p + 40, 0x000000ffu); // ZCmpCaps: all
    wr32(p + 44, 0x00003fffu); // SrcBlendCaps: all, BLENDFACTOR
    wr32(p + 48, 0x00003fffu); // DestBlendCaps
    wr32(p + 52, 0x000000ffu); // AlphaCmpCaps
    wr32(p + 56, 0x00084208u); // ShadeCaps
    wr32(
        p + 60,
        0x0001ec45u); // TextureCaps: PERSPECTIVE, ALPHA, CUBEMAP, MIPMAP, MIPCUBEMAP, PROJECTED, VOLUMEMAP, ...
    wr32(
        p + 64,
        0x03030700u); // TextureFilterCaps: MIN point/linear/aniso, MIP point/linear, MAG point/linear
    wr32(p + 68, 0x03030300u);  // CubeTextureFilterCaps
    wr32(p + 72, 0x03030300u);  // VolumeTextureFilterCaps
    wr32(p + 76, 0x0000003fu);  // TextureAddressCaps
    wr32(p + 80, 0x0000003fu);  // VolumeTextureAddressCaps
    wr32(p + 84, 0x0000001fu);  // LineCaps
    wr32(p + 88, 4096);         // MaxTextureWidth
    wr32(p + 92, 4096);         // MaxTextureHeight
    wr32(p + 96, 512);          // MaxVolumeExtent
    wr32(p + 100, 8192);        // MaxTextureRepeat
    wr32(p + 104, 8192);        // MaxTextureAspectRatio
    wr32(p + 108, 16);          // MaxAnisotropy
    f(112, 1e10f);              // MaxVertexW
    f(116, -1e8f);              // GuardBandLeft
    f(120, -1e8f);              // GuardBandTop
    f(124, 1e8f);               // GuardBandRight
    f(128, 1e8f);               // GuardBandBottom
    wr32(p + 136, 0x000001ffu); // StencilCaps: all ops, TWOSIDED
    wr32(p + 140, 0x00080008u); // FVFCaps: 8 texture coordinates, PSIZE
    wr32(p + 144, 0x03feffffu); // TextureOpCaps
    wr32(p + 148, 8);           // MaxTextureBlendStages
    wr32(p + 152, 8);           // MaxSimultaneousTextures
    wr32(p + 156, 0x0000007bu); // VertexProcessingCaps
    wr32(p + 160, 8);           // MaxActiveLights
    wr32(p + 164, 6);           // MaxUserClipPlanes
    wr32(p + 168, 4);           // MaxVertexBlendMatrices
    f(176, 8192.0f);            // MaxPointSize
    wr32(p + 180, 1048575);     // MaxPrimitiveCount
    wr32(p + 184, 1048575);     // MaxVertexIndex
    wr32(p + 188, 16);          // MaxStreams
    wr32(p + 192, 255);         // MaxStreamStride
    wr32(p + 196, 0xfffe0300u); // VertexShaderVersion vs_3_0
    wr32(p + 200, 256);         // MaxVertexShaderConst
    wr32(p + 204, 0xffff0300u); // PixelShaderVersion ps_3_0
    f(208, 3.4e38f);            // PixelShaderMaxValue
    wr32(
        p + 212,
        0x00000051u); // DevCaps2: STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES, VERTEXELEMENTSCANSHARESTREAMOFFSET
    wr32(p + 232, 1);           // NumberOfAdaptersInGroup
    wr32(p + 236, 0x0000030fu); // DeclTypes
    wr32(p + 240, 4);           // NumSimultaneousRTs
    wr32(p + 244, 0x03000300u); // StretchRectFilterCaps: point, linear
    wr32(p + 248, 1);           // VS20Caps.Caps: PREDICATION
    wr32(p + 252, 24);          // VS20Caps.DynamicFlowControlDepth
    wr32(p + 256, 32);          // VS20Caps.NumTemps
    wr32(p + 260, 4);           // VS20Caps.StaticFlowControlDepth
    wr32(p + 264, 0x1f);        // PS20Caps.Caps
    wr32(p + 268, 24);          // PS20Caps.DynamicFlowControlDepth
    wr32(p + 272, 32);          // PS20Caps.NumTemps
    wr32(p + 276, 4);           // PS20Caps.StaticFlowControlDepth
    wr32(p + 280, 512);         // PS20Caps.NumInstructionSlots
    wr32(p + 284, 0x00000300u); // VertexTextureFilterCaps
    wr32(p + 288, 0xffffffffu); // MaxVShaderInstructionsExecuted
    wr32(p + 292, 0xffffffffu); // MaxPShaderInstructionsExecuted
    wr32(p + 296, 32768);       // MaxVertexShader30InstructionSlots
    wr32(p + 300, 32768);       // MaxPixelShader30InstructionSlots
}
void D9_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 3));
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterMonitor(X86 *c) {
    set_eax(c, 1); // a non-null HMONITOR
}

extern "C" void dinput_log_census(void);

// Once-a-second pace record. Scarface clamps animation time to at least 16 ms
// inside sub_40D670, so the guest Present rate is the animation rate.
static uint32_t g_pace_interval = 0xffffffffu;
static int g_pace_windowed = -1;
static uint32_t g_pace_presents = 0;
static uint64_t g_pace_window_ns = 0;
static uint64_t g_pace_last_present_ns = 0;
static uint32_t g_pace_window_presents = 0;
static uint32_t g_pace_gap_us[8];
static uint32_t g_pace_gap_i = 0;
static uint32_t g_pace_gap_n = 0;
static uint32_t g_pace_lines = 0;
static uint64_t g_pace_origin_ns = 0;
static uint64_t g_pace_deadline_ns = 0;

static const char *pace_interval_name(uint32_t interval) {
    switch (interval) {
    case 0:
        return "DEFAULT";
    case 1:
        return "ONE";
    case 2:
        return "TWO";
    case 4:
        return "THREE";
    case 8:
        return "FOUR";
    case 0x80000000u:
        return "IMMEDIATE";
    default:
        return "OTHER";
    }
}

static void pace_log_second(uint64_t now) {
    uint32_t min_us = 0, max_us = 0;
    for (uint32_t i = 0; i < g_pace_gap_n; i++) {
        if (i == 0 || g_pace_gap_us[i] < min_us)
            min_us = g_pace_gap_us[i];
        if (g_pace_gap_us[i] > max_us)
            max_us = g_pace_gap_us[i];
    }
    float flow = 0;
    bool flow_ok = false;
    if (gm_valid(0x80AC04u, 4)) {
        uint32_t obj = rd32(0x80AC04u);
        if (obj && gm_valid(obj + 24, 4)) {
            uint32_t bits = rd32(obj + 24);
            memcpy(&flow, &bits, 4);
            flow_ok = true;
        }
    }
    double sec = (now - g_pace_window_ns) / 1e9;
    if (sec < 0.001)
        sec = 0.001;
    LOGW("pace: t=%.1fs guest_presents=%u +%u (%.0f/s) interval=%s(0x%x) windowed=%d "
         "gap_us min=%u max=%u n=%u flow_ms=%s%.3f",
         (now - g_pace_origin_ns) / 1e9, g_pace_presents, g_pace_window_presents,
         g_pace_window_presents / sec, pace_interval_name(g_pace_interval), g_pace_interval,
         g_pace_windowed, min_us, max_us, g_pace_gap_n, flow_ok ? "" : "unread ", flow);
    fflush(stderr);
    g_pace_window_ns = now;
    g_pace_window_presents = 0;
    g_pace_gap_n = 0;
    g_pace_gap_i = 0;
    if (++g_pace_lines == 8)
        dinput_log_census();
}

// D3D9 Present with INTERVAL_ONE blocks until the next vblank. Scarface asks
// for ONE. DEFAULT (0) is defined as ONE. The animation step in sub_40D670
// then treats anything under 16 ms as 16 ms, which is one 60 Hz frame.
static void pace_wait_interval() {
    uint32_t blanks = 1;
    if (g_pace_interval == 0x80000000u)
        return;
    if (g_pace_interval == 2)
        blanks = 2;
    else if (g_pace_interval == 4)
        blanks = 3;
    else if (g_pace_interval == 8)
        blanks = 4;
    else if (g_pace_interval != 0 && g_pace_interval != 1)
        return;
    const uint64_t step = blanks * 16666667ull;
    uint64_t now = os_monotonic_ns();
    if (!g_pace_deadline_ns || now > g_pace_deadline_ns + step * 2)
        g_pace_deadline_ns = now;
    g_pace_deadline_ns += step;
    if (g_pace_deadline_ns <= now)
        return;
    uint64_t wait = g_pace_deadline_ns - now;
    timespec ts;
    ts.tv_sec = (time_t)(wait / 1000000000ull);
    ts.tv_nsec = (long)(wait % 1000000000ull);
    nanosleep(&ts, nullptr);
}

static void pace_on_present() {
    pace_wait_interval();
    uint64_t now = os_monotonic_ns();
    if (!g_pace_origin_ns)
        g_pace_origin_ns = now;
    if (g_pace_last_present_ns) {
        uint64_t gap = (now - g_pace_last_present_ns) / 1000ull;
        if (gap > 60000000ull)
            gap = 60000000ull;
        g_pace_gap_us[g_pace_gap_i++ % 8] = (uint32_t)gap;
        if (g_pace_gap_n < 8)
            g_pace_gap_n++;
    }
    g_pace_last_present_ns = now;
    g_pace_presents++;
    g_pace_window_presents++;
    if (!g_pace_window_ns)
        g_pace_window_ns = now;
    else if (now - g_pace_window_ns >= 1000000000ull)
        pace_log_second(now);
}

void D9_CreateDevice(X86 *c) {
    ComObj *d3d = this_d3d9(c);
    uint32_t present = arg(c, 5), out = arg(c, 6);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_out_ptr(out, 0);
    ComObj *dev = com_new(K_D3D9DEVICE);
    if (!dev) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    dev->dev_d3d = d3d->id;
    // D3DPRESENT_PARAMETERS: back buffer size and format first, the device
    // window at 28 and Windowed at 32.
    if (present) {
        dev->width = rd32(present + 0);
        dev->height = rd32(present + 4);
        dev->bpp = 32;
        dev->hwnd = rd32(present + 28);
        dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    }
    uint32_t view = com_view(dev, IF_D3DDEVICE9);
    if (!view) {
        com_release(dev);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    {
        ComObj *bb = device_backbuffer(dev);
        D9Pipeline &pl = d9_pipeline(dev->id);
        pl.windowed = !present || rd32(present + 32) != 0;
        g_pace_windowed = pl.windowed ? 1 : 0;
        g_pace_interval = present ? rd32(present + 52) : 0;
        LOGW("pace: CreateDevice %ux%u windowed=%d PresentationInterval=%s (0x%x)", dev->width,
             dev->height, g_pace_windowed, pace_interval_name(g_pace_interval), g_pace_interval);
        pl.focus_loss_seen = g_d3d9_focus_loss_epoch.load();
        pl.reset_needed = !pl.windowed && !g_d3d9_focused.load();
        pl.last_cooperative_status = D3D_OK9;
        g_render_device_id.store(dev->id);
        g_render_device_live.store(true);
        pl.color_target[0] = bb ? bb->id : 0;
        pl.scissor[2] = (int32_t)dev->width;
        pl.scissor[3] = (int32_t)dev->height;
        if (present && rd32(present + 36)) {
            device_depthbuffer(dev);
            pl.rs[7] = 1; // ZENABLE defaults on with an automatic depth buffer
            pl.states_changed();
        }
    }
    LOGW("d3d9: CreateDevice %ux%u, window %08x, %s renderer", dev->width, dev->height, dev->hwnd,
         host_d9_active() ? "GPU" : "CPU");
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);

// ---------------------------------------------------------------------------
// IDirect3DDevice9
//
// Every slot is named and in its interface order, because a guest calls them
// by index. A stub reports itself once and returns D3D_OK: a game that is
// told "no" by a method it expects to work usually stops, and stopping tells
// us less than carrying on does.
// ---------------------------------------------------------------------------
#define D9_STUB(name, argc)                                                                        \
    void Dev_##name(X86 *c) {                                                                      \
        log_once("d3d9.dev." #name, "d3d9: IDirect3DDevice9::" #name " is not implemented");       \
        (void)argc;                                                                                \
        com_ret(c, D3D_OK9);                                                                       \
    }

D9_STUB(EvictManagedResources, 1)
D9_STUB(GetDisplayMode, 3)
D9_STUB(GetCreationParameters, 2)
D9_STUB(SetCursorProperties, 4)
D9_STUB(SetCursorPosition, 4)
D9_STUB(ShowCursor, 2)
D9_STUB(CreateAdditionalSwapChain, 3)
D9_STUB(GetSwapChain, 3)
D9_STUB(GetRasterStatus, 3)
D9_STUB(SetDialogBoxMode, 2)
D9_STUB(SetGammaRamp, 4)
D9_STUB(GetGammaRamp, 3)
D9_STUB(CreateVolumeTexture, 9)
D9_STUB(UpdateSurface, 5)
D9_STUB(UpdateTexture, 3)
D9_STUB(GetRenderTargetData, 3)
D9_STUB(GetFrontBufferData, 3)
D9_STUB(ColorFill, 4)
D9_STUB(MultiplyTransform, 3)
D9_STUB(SetMaterial, 2)
D9_STUB(GetMaterial, 2)
// D3DLIGHT9 is 104 bytes in the guest's 32-bit ABI. The game saves the
// device's light descriptions and feeds them to its own vertex shaders, so
// returning success without filling GetLight's output corrupts their colors.
void Dev_SetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), in = arg(c, 2);
    if (!dev || !in || !gm_valid(in, 104) || rd32(in) < 1 || rd32(in) > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline::Light &light = d9_pipeline(dev->id).lights[index];
    memcpy(light.bytes, gm_ptr(in), sizeof light.bytes);
    if (d3d_color_trace()) {
        static uint32_t reported = 0;
        if (reported++ < 16)
            LOGW("d3d9 color trace: SetLight ret=%08x index=%u type=%u diffuse=%08x,%08x,%08x direction=%08x,%08x,%08x",
                 rd32(c->r[R_ESP]), index, rd32(in), rd32(in + 4), rd32(in + 8),
                 rd32(in + 12), rd32(in + 64), rd32(in + 68), rd32(in + 72));
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 104)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    auto &lights = d9_pipeline(dev->id).lights;
    auto it = lights.find(index);
    if (it == lights.end()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(out), it->second.bytes, sizeof it->second.bytes);
    if (d3d_color_trace()) {
        static uint32_t reported = 0;
        if (reported++ < 16)
            LOGW("d3d9 color trace: GetLight ret=%08x index=%u out=%08x type=%u diffuse=%08x,%08x,%08x",
                 rd32(c->r[R_ESP]), index, out, rd32(out), rd32(out + 4), rd32(out + 8),
                 rd32(out + 12));
    }
    com_ret(c, D3D_OK9);
}
void Dev_LightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t index = arg(c, 1);
    auto &lights = d9_pipeline(dev->id).lights;
    auto [it, inserted] = lights.try_emplace(index);
    if (inserted) {
        // D3D9 creates this directional light when no SetLight preceded it.
        uint32_t type = 3, one = 0x3f800000u;
        memcpy(it->second.bytes, &type, 4);
        for (uint32_t off : {4u, 8u, 12u, 72u})
            memcpy(it->second.bytes + off, &one, 4);
    }
    it->second.enabled = arg(c, 2) != 0;
    com_ret(c, D3D_OK9);
}
void Dev_GetLightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    auto &lights = d9_pipeline(dev->id).lights;
    auto it = lights.find(index);
    if (it == lights.end()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, it->second.enabled ? 1u : 0u);
    com_ret(c, D3D_OK9);
}
D9_STUB(SetClipPlane, 3)
D9_STUB(GetClipPlane, 3)
void Dev_GetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!dev || state >= 256 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t value = d9_pipeline(dev->id).rs[state];
    if (d3d_rs_trace()) {
        static std::atomic<uint32_t> trace_count{0};
        uint32_t n = trace_count.fetch_add(1);
        if (n < 64 && (state == 7 || state == 14 || state == 15 || state == 19 ||
                       state == 20 || state == 27 || state == 28))
            LOGW("d3d9 rs trace: GET ret=%08x device=%u state=%u current=%08x out=%08x before=%08x after=%08x",
                 rd32(c->r[R_ESP]), dev->id, state, value, out, rd32(out), value);
    }
    wr32(out, value);
    com_ret(c, D3D_OK9);
}
D9_STUB(CreateStateBlock, 3)
D9_STUB(BeginStateBlock, 1)
D9_STUB(EndStateBlock, 2)
D9_STUB(SetClipStatus, 2)
D9_STUB(GetClipStatus, 2)
D9_STUB(GetTexture, 3)
D9_STUB(GetTextureStageState, 4)
D9_STUB(GetSamplerState, 4)
D9_STUB(ValidateDevice, 2)
D9_STUB(SetPaletteEntries, 3)
D9_STUB(GetPaletteEntries, 3)
D9_STUB(SetCurrentTexturePalette, 2)
D9_STUB(GetCurrentTexturePalette, 2)
D9_STUB(GetScissorRect, 2)
D9_STUB(SetSoftwareVertexProcessing, 2)
D9_STUB(GetSoftwareVertexProcessing, 1)
D9_STUB(SetNPatchMode, 2)
D9_STUB(GetNPatchMode, 1)
D9_STUB(ProcessVertices, 6)
D9_STUB(GetVertexDeclaration, 2)
void Dev_SetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    d9_pipeline(dev->id).fvf = arg(c, 1);
    if (d9_vertex_trace())
        LOGW("d3d9 vertex trace: SetFVF return=%08x fvf=%08x decl=%u",
             rd32(c->r[R_ESP]), arg(c, 1), d9_pipeline(dev->id).declaration);
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        LOGW("d3d9: SetFVF(%08x)", arg(c, 1));
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_fits(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).fvf);
    com_ret(c, D3D_OK9);
}
static bool copy_shader_bytecode(uint32_t addr, std::vector<uint8_t> *out);
void Dev_CreateVertexShader(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t fn = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_fits(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    std::vector<uint8_t> bytes;
    if (!copy_shader_bytecode(fn, &bytes)) {
        log_once("d3d9.cvs.bad", "d3d9: CreateVertexShader rejected bytecode at %08x", fn);
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const d9sh::Program &prog = d9sh::program_for(bytes);
    if (!prog.ok || prog.pixel) {
        log_once("d3d9.cvs.reject", "d3d9: CreateVertexShader decoder: %s",
                 prog.ok ? "not a vertex shader" : prog.why.c_str());
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *sh = com_new(K_D3D9VS);
    if (!sh) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    sh->dev_d3d = dev->id;
    sh->blob = std::move(bytes);
    uint32_t view = com_view(sh, IF_D3DVERTEXSHADER9);
    if (!view) {
        com_release(sh);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    if (d9_vertex_trace())
        LOGW("d3d9 vertex trace: CreateVertexShader return=%08x input=%08x version=%08x out=%08x object=%u view=%08x bytes=%zu",
             rd32(c->r[R_ESP]), fn, rd32(fn), out, sh->id, view, sh->blob.size());
    com_ret(c, D3D_OK9);
}
void Dev_SetVertexShader(X86 *c) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    uint32_t argp = arg(c, 1), prev = pl.vertex_shader;
    ComObj *sh = argp ? com_this(argp) : nullptr;
    if (argp && !(sh && sh->kind == K_D3D9VS)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    pl.vertex_shader_set = true;
    pl.vertex_shader_arg = argp;
    if (!sh || sh->id != prev) {
        if (sh) {
            pl.vs.bytes = std::make_shared<const std::vector<uint8_t>>(sh->blob);
            pl.vs_key = 0;
            com_addref(sh);
            pl.vertex_shader = sh->id;
        } else {
            pl.vs.bytes.reset();
            pl.vs_key = 0;
            pl.vertex_shader = 0;
        }
        if (prev)
            if (ComObj *old = com_get(prev))
                com_release(old);
    }
    if (d9_vertex_trace()) {
        static uint32_t traced = 0;
        if (traced++ < 64)
            LOGW("d3d9 vertex trace: SetVertexShader return=%08x arg=%08x object=%u bytes=%zu",
                 rd32(c->r[R_ESP]), argp, pl.vertex_shader, pl.vs.size());
    }
    com_ret(c, D3D_OK9);
}
D9_STUB(GetVertexShaderConstantF, 4)
D9_STUB(SetVertexShaderConstantI, 4)
D9_STUB(GetVertexShaderConstantI, 4)
D9_STUB(SetVertexShaderConstantB, 4)
D9_STUB(GetVertexShaderConstantB, 4)
D9_STUB(GetStreamSource, 5)
D9_STUB(SetStreamSourceFreq, 3)
D9_STUB(GetStreamSourceFreq, 3)
D9_STUB(GetIndices, 2)
static bool copy_shader_bytecode(uint32_t addr, std::vector<uint8_t> *out) {
    out->clear();
    if (!addr)
        return false;
    for (uint32_t i = 0; i < 4096; ++i) {
        if (!gm_fits(addr + i * 4u, 4))
            return false;
        uint32_t t = rd32(addr + i * 4u);
        uint8_t b[4];
        memcpy(b, &t, 4);
        out->insert(out->end(), b, b + 4);
        if (i > 0 && t == d9sh::OP_END)
            return true;
    }
    return false;
}

void Dev_CreatePixelShader(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t fn = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_fits(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    std::vector<uint8_t> bytes;
    if (!copy_shader_bytecode(fn, &bytes)) {
        log_once("d3d9.cps.bad", "d3d9: CreatePixelShader rejected bytecode at %08x", fn);
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const d9sh::Program &prog = d9sh::program_for(bytes);
    if (!prog.ok || !prog.pixel) {
        log_once("d3d9.cps.reject", "d3d9: CreatePixelShader decoder: %s",
                 prog.ok ? "not a pixel shader" : prog.why.c_str());
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *sh = com_new(K_D3D9PS);
    if (!sh) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    sh->dev_d3d = dev->id;
    sh->blob = std::move(bytes);
    uint32_t view = com_view(sh, IF_D3DPIXELSHADER9);
    if (!view) {
        com_release(sh);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    uint32_t ver = rd32(fn);
    char key[64];
    snprintf(key, sizeof key, "d3d9.cps.%08x.%u", ver, (unsigned)sh->blob.size());
    log_once(key, "d3d9: CreatePixelShader version %08x, %u bytes -> %08x", ver,
             (unsigned)sh->blob.size(), view);
    if (recomp_post_intro_after_thx())
        log_once("hud.cps.post-thx", "hud: first post-THX CreatePixelShader");
    if (recomp_post_intro_after_thx() && log_once("hud.cps.dump", "hud: dump from CreatePixelShader"))
        recomp_post_intro_dump("post-thx-CreatePixelShader");
    com_ret(c, D3D_OK9);
}

void Dev_SetPixelShader(X86 *c) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    uint32_t prev = pl.pixel_shader;
    uint32_t argp = arg(c, 1);
    ComObj *sh = argp ? com_this(argp) : nullptr;
    if (argp && !(sh && sh->kind == K_D3D9PS)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (sh && sh->id == prev) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (sh) {
        pl.ps.bytes = std::make_shared<const std::vector<uint8_t>>(sh->blob);
        pl.ps_key = 0;
        com_addref(sh);
        pl.pixel_shader = sh->id;
    } else {
        pl.ps.bytes.reset();
        pl.ps_key = 0;
        pl.pixel_shader = 0;
    }
    if (prev) {
        if (ComObj *old = com_get(prev))
            com_release(old);
    }
    static int logged = 0;
    if (logged < 24) {
        ++logged;
        LOGW("d3d9: SetPixelShader(%08x) obj=%u bytes=%zu (was %u)", argp, pl.pixel_shader,
             pl.ps.size(), prev);
    }
    com_ret(c, D3D_OK9);
}
D9_STUB(GetPixelShaderConstantF, 4)
D9_STUB(SetPixelShaderConstantI, 4)
D9_STUB(GetPixelShaderConstantI, 4)
D9_STUB(SetPixelShaderConstantB, 4)
D9_STUB(GetPixelShaderConstantB, 4)
// GetVertexShader / GetPixelShader write the bound interface and AddRef it.
void Dev_GetVertexShader(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *sh = pl.vertex_shader ? com_get(pl.vertex_shader) : nullptr;
    if (!(sh && sh->kind == K_D3D9VS)) {
        wr32(out, 0);
        com_ret(c, D3D_OK9);
        return;
    }
    com_addref(sh);
    wr32(out, com_view(sh, IF_D3DVERTEXSHADER9));
    com_ret(c, D3D_OK9);
}
void Dev_GetPixelShader(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out || !gm_fits(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *sh = pl.pixel_shader ? com_get(pl.pixel_shader) : nullptr;
    if (!(sh && sh->kind == K_D3D9PS)) {
        wr32(out, 0);
        com_ret(c, D3D_OK9);
        return;
    }
    com_addref(sh);
    wr32(out, com_view(sh, IF_D3DPIXELSHADER9));
    com_ret(c, D3D_OK9);
}

void PS_GetFunction(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t data = arg(c, 1), sizep = arg(c, 2);
    if (!o || o->kind != K_D3D9PS || !sizep || !gm_fits(sizep, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t n = (uint32_t)o->blob.size();
    uint32_t have = rd32(sizep);
    wr32(sizep, n);
    if (!data) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (have < n || !gm_fits(data, n)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(data), o->blob.data(), n);
    com_ret(c, D3D_OK9);
}

void VS_GetFunction(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t data = arg(c, 1), sizep = arg(c, 2);
    if (!o || o->kind != K_D3D9VS || !sizep || !gm_fits(sizep, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t n = (uint32_t)o->blob.size();
    uint32_t have = rd32(sizep);
    wr32(sizep, n);
    if (!data) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (have < n || !gm_fits(data, n)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(data), o->blob.data(), n);
    com_ret(c, D3D_OK9);
}

D9_STUB(DrawRectPatch, 4)
D9_STUB(DrawTriPatch, 4)
D9_STUB(DeletePatch, 2)

// (this, pRect)
void Dev_SetScissorRect(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t r = arg(c, 1);
    if (dev && r)
        for (int i = 0; i < 4; ++i)
            d9_pipeline(dev->id).scissor[i] = (int32_t)rd32(r + 4u * (uint32_t)i);
    com_ret(c, dev && r ? D3D_OK9 : D3DERR_INVALIDCALL);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void device_forget_buffers(ComObj *dev);

// (this, pPresentationParameters): new back buffer and depth buffer at the
// new size; every resource the game made itself survives, as with
// D3DPOOL_MANAGED.
void Dev_Reset(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t present = arg(c, 1);
    if (!dev || !present) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (!d9_pipeline(dev->id).windowed && !g_d3d9_focused.load()) {
        com_ret(c, D3DERR_DEVICELOST);
        return;
    }
    const uint64_t focus_loss_epoch = g_d3d9_focus_loss_epoch.load();
    dev->width = rd32(present + 0);
    dev->height = rd32(present + 4);
    dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    device_forget_buffers(dev);
    ComObj *bb = device_backbuffer(dev);
    dev->render_target = bb ? bb->id : 0;
    D9Pipeline &pl = d9_pipeline(dev->id);
    pl.windowed = rd32(present + 32) != 0;
    pl.focus_loss_seen = focus_loss_epoch;
    pl.reset_needed = false;
    pl.last_cooperative_status = D3D_OK9;
    pl.color_target[0] = dev->render_target;
    for (int i = 1; i < 4; ++i)
        pl.color_target[i] = 0;
    pl.viewport_set = false;
    pl.scissor[0] = pl.scissor[1] = 0;
    pl.scissor[2] = (int32_t)dev->width;
    pl.scissor[3] = (int32_t)dev->height;
    if (rd32(present + 36)) { // EnableAutoDepthStencil
        device_depthbuffer(dev);
        pl.rs[7] = 1;
        pl.states_changed();
    } else
        dev->zbuffer_obj = 0;
    LOGW("d3d9: Reset to %ux%u", dev->width, dev->height);
    com_ret(c, D3D_OK9);
}

void Dev_TestCooperativeLevel(X86 *c) {
    ComObj *dev = this_device9(c);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    const uint32_t status = fullscreen_cooperative_status(pl);
    if (status != pl.last_cooperative_status) {
        LOGW("d3d9: TestCooperativeLevel %08x (focus=%d reset_needed=%d)", status,
             g_d3d9_focused.load(), pl.reset_needed);
        pl.last_cooperative_status = status;
    }
    com_ret(c, status);
}
void Dev_GetAvailableTextureMem(X86 *c) {
    set_eax(c, 256u * 1024u * 1024u);
}
void Dev_GetNumberOfSwapChains(X86 *c) {
    set_eax(c, 1);
}
void Dev_GetDirect3D(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d3d = dev ? com_get(dev->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(d3d, IF_D3D9);
    com_addref(d3d);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}
void Dev_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 1));
    com_ret(c, D3D_OK9);
}
void Dev_BeginScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = true;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_EndScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = false;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// Defined with the resources below.
static ComObj *device_backbuffer(ComObj *dev);
static uint8_t *resource_bytes(ComObj *o);

// (this, Count, pRects, Flags, Color, Z, Stencil). Only the colour target is
// real so far: a 32-bit surface is filled with the colour, over the listed
// D3DRECTs or the whole surface.
void Dev_Clear(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        HostD9Target target = gpu_target(dev);
        int32_t vp[4];
        target_viewport(dev, target, vp);
        std::vector<int32_t> rs;
        if (count && rects && gm_fits(rects, count * 16u))
            for (uint32_t i = 0; i < count * 4; ++i)
                rs.push_back((int32_t)rd32(rects + 4 * i));
        float z;
        uint32_t zbits = arg(c, 5);
        memcpy(&z, &zbits, 4);
        host_d9_clear(&target, vp, (uint32_t)(rs.size() / 4), rs.empty() ? nullptr : rs.data(),
                      flags, color, z, arg(c, 6));
        gpu_target_drawn(dev, flags & 1u, flags & 6u);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *s = com_get(dev->render_target);
    if (!s)
        s = device_backbuffer(dev);
    static uint32_t g_clears = 0;
    if (++g_clears <= 24)
        LOGW("d3d9: clear %u: flags %x colour %08x rects %u, target %u (%ux%u, %u bpp)%s", g_clears,
             flags, color, count, s ? s->id : 0, s ? s->width : 0, s ? s->height : 0,
             s ? s->bpp : 0, (s && s->id == dev->palette_obj) ? ", the back buffer" : "");
    uint8_t *bytes = resource_bytes(s);
    if ((flags & 1u) && bytes && s->bpp == 32) {
        uint32_t n = (count && rects) ? count : 1;
        for (uint32_t r = 0; r < n; ++r) {
            int32_t x1 = 0, y1 = 0, x2 = (int32_t)s->width, y2 = (int32_t)s->height;
            if (count && rects) {
                uint32_t q = rects + r * 16u;
                x1 = std::max(x1, (int32_t)rd32(q));
                y1 = std::max(y1, (int32_t)rd32(q + 4));
                x2 = std::min(x2, (int32_t)rd32(q + 8));
                y2 = std::min(y2, (int32_t)rd32(q + 12));
            }
            for (int32_t y = y1; y < y2; ++y) {
                uint8_t *row = bytes + (size_t)y * s->pitch;
                for (int32_t x = x1; x < x2; ++x)
                    memcpy(row + 4 * x, &color, 4);
            }
        }
        d9_raster_invalidate(s->id);
    }
    com_ret(c, D3D_OK9);
}

// (this, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion). The back
// buffer goes to the host as a 32-bit frame: the first thing this module puts
// on screen.
void Dev_Present(X86 *c) {
    pace_on_present();
    ComObj *dev = this_device9(c);
    ComObj *bb = dev ? device_backbuffer(dev) : nullptr;
    if (bb && gpu_on()) {
        gpu_sync(bb);
        ddraw_external_present_begin();
        host_present(nullptr, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        host_d9_present(bb->id, bb->width, bb->height);
        ddraw_external_present_end();
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *bbytes = resource_bytes(bb);
    if (bb && bbytes) {
        uint32_t lit = 0, n = bb->width * bb->height;
        for (uint32_t i = 0; i < n; i += 97) {
            uint32_t v;
            memcpy(&v, bbytes + (size_t)i * 4, 4);
            if (v & 0x00ffffffu)
                ++lit;
        }
        LOGW("d3d9: present back buffer %u (%ux%u, %u bpp), %u of %u sampled pixels lit, target "
             "now %u",
             bb->id, bb->width, bb->height, bb->bpp, lit, (n + 96) / 97, dev->render_target);
    }
    if (bb && bbytes && bb->bpp == 32) {
        ddraw_external_present_begin();
        host_present(bbytes, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        ddraw_external_present_end();
    }
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// Device resources
//
// A game creates textures, buffers and surfaces before it draws anything, and
// then writes its geometry and its images into them. None of that reaches a
// GPU yet, but the storage is real guest memory, so a Lock hands back a
// pointer the game can fill and the contents survive to be drawn later.
// ---------------------------------------------------------------------------
// A resource's contents live in host memory, as a driver's would: the guest
// heap is the game's, and a texture set would exhaust it. The guest sees the
// bytes only while the resource is locked, through a copy made on the first
// lock and written back on the last unlock.
static uint32_t resource_alloc(ComObj *o, uint32_t bytes) {
    if (!bytes)
        return 0;
    o->blob.assign(bytes, 0);
    o->pixels = 0;
    o->pixels_bytes = bytes;
    return 1;
}

// The bytes to read or write right now: the guest copy while locked.
static uint8_t *resource_bytes(ComObj *o) {
    if (!o)
        return nullptr;
    if (o->pixels)
        return gm_ptr(o->pixels);
    return o->blob.empty() ? nullptr : o->blob.data();
}

static void gpu_before_lock(ComObj *s);
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi);
static void gpu_forget(ComObj *o);

static uint32_t stage_lock(ComObj *o) {
    if (!o || o->blob.empty())
        return 0;
    if (o->lock_count == 0)
        gpu_before_lock(o);
    if (o->lock_count++ == 0) {
        o->pixels = heap_alloc((uint32_t)o->blob.size(), false, 16);
        if (!o->pixels) {
            o->lock_count = 0;
            return 0;
        }
        memcpy(gm_ptr(o->pixels), o->blob.data(), o->blob.size());
    }
    return o->pixels;
}

static void stage_unlock(ComObj *o) {
    if (!o || o->lock_count <= 0)
        return;
    if (--o->lock_count == 0 && o->pixels) {
        memcpy(o->blob.data(), gm_ptr(o->pixels), o->blob.size());
        heap_free(o->pixels);
        o->pixels = 0;
        d9_raster_invalidate(o->id);
        gpu_after_unlock(o, 0, (uint32_t)o->blob.size());
    }
}

static void d3d9_resource_destroy(ComObj *o) {
    if (o->pixels)
        heap_free(o->pixels);
    o->pixels = 0;
    o->lock_count = 0;
    o->pixels_bytes = 0;
    std::vector<uint8_t>().swap(o->blob);
    d9_raster_invalidate(o->id);
    gpu_forget(o);
}

// Bytes per pixel for the formats this game creates. D3DFMT_A8R8G8B8 is 21,
// X8R8G8B8 22, R5G6B5 23, A8 28, D24S8 75, and the DXT block formats carry
// their four characters.
static uint32_t format_bytes(uint32_t fmt) {
    switch (fmt) {
    case 27:
    case 28:
    case 41:
    case 50:
    case 52: // R3G3B2, A8, P8, L8, A4L4
        return 1;
    case 23:
    case 24:
    case 25:
    case 26:
    case 29:
    case 30:
    case 40:
    case 51:
    case 60:
    case 61:
    case 70:
    case 73:
    case 80:
    case 81:
    case 111:
        return 2;
    case 36:
    case 113: // A16B16G16R16, A16B16G16R16F
        return 8;
    case 116: // A32B32G32R32F
        return 16;
    default:
        return 4;
    }
}

// The DXT formats, stored as blocks of 4x4 pixels.
static uint32_t dxt_block_bytes(uint32_t fmt) {
    switch (fmt) {
    case 0x31545844u:
        return 8; // DXT1
    case 0x32545844u:
    case 0x33545844u:
    case 0x34545844u:
    case 0x35545844u:
        return 16;
    default:
        return 0;
    }
}

static ComObj *make_surface(ComObj *dev, uint32_t w, uint32_t h, uint32_t fmt) {
    ComObj *s = com_new(K_D3D9SURFACE);
    if (!s)
        return nullptr;
    s->dev_d3d = dev ? dev->id : 0;
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->rmask = fmt; // the D3DFORMAT, which a Direct3D 9 surface has no other field for
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // A compressed surface is rows of blocks; Pitch is the bytes in one.
        uint32_t bw = (s->width + 3) / 4, bh = (s->height + 3) / 4;
        s->bpp = 0;
        s->pitch = bw * block;
        resource_alloc(s, s->pitch * bh);
    } else {
        s->bpp = format_bytes(fmt) * 8;
        s->pitch = s->width * format_bytes(fmt);
        resource_alloc(s, s->pitch * s->height);
    }
    return s;
}

// ---- What the CPU renderer needs to know about surfaces and textures --------
bool d9_surface_info(uint32_t surface_id, D9SurfaceInfo *out) {
    ComObj *s = com_get(surface_id);
    if (!s || s->kind != K_D3D9SURFACE)
        return false;
    out->data = resource_bytes(s);
    out->size = s->blob.size();
    out->width = s->width;
    out->height = s->height;
    out->pitch = s->pitch;
    out->format = s->rmask;
    return true;
}

uint32_t d9_texture_level0(uint32_t texture_id) {
    ComObj *t = com_get(texture_id);
    if (!t)
        return 0;
    if (t->kind == K_D3D9SURFACE)
        return t->id;
    if (t->kind != K_D3D9TEXTURE)
        return 0;
    if (t->caps) // a cube: face zero, level zero, once it exists
        return t->surfaces.empty() ? 0 : t->surfaces[0];
    return t->back_obj;
}

// The surface behind a 2D texture's level, or null.
static ComObj *texture_level(ComObj *tex, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || tex->caps)
        return nullptr;
    if (level < tex->surfaces.size())
        return com_get(tex->surfaces[level]);
    return level == 0 ? com_get(tex->back_obj) : nullptr;
}

// ---- The GPU's copies ------------------------------------------------------
// With a GPU renderer running, every texture, surface and buffer has a mirror
// on the host under the object's id; a surface that belongs to a texture is a
// slot of its container's mirror. The shim's bytes stay the reference for what
// the game wrote, the GPU's for what was drawn: a slot the game unlocked is
// uploaded before the next draw that reads it, and a slot the GPU drew into is
// read back before the game locks it.
static const uint32_t USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2;

static unsigned g_lock_trace_budget = 0, g_upload_trace_budget = 0;

struct GpuMirror {
    bool defined = false;
    std::vector<uint8_t> cpu_dirty;      // by slot
    std::vector<uint8_t> gpu_newer;      // by slot
    uint32_t dirty_lo = 0, dirty_hi = 0; // buffers: the changed byte range
};
static std::unordered_map<uint32_t, GpuMirror> &gpu_mirrors() {
    static auto *m = new std::unordered_map<uint32_t, GpuMirror>();
    return *m;
}
// D3DUSAGE of every texture and surface, by id.
static std::unordered_map<uint32_t, uint32_t> &resource_usage() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static bool gpu_on() {
    return host_d9_active() != 0;
}
static bool is_depth_format(uint32_t fmt) {
    return (fmt >= 70 && fmt <= 82) || fmt == 0x5a544e49u /* INTZ */ ||
           fmt == 0x34324644u /* DF24 */ || fmt == 0x36314644u /* DF16 */;
}

// A surface's place in its mirror: the container's id and the slot, or the
// surface's own id and slot zero.
static uint32_t mirror_slot(ComObj *s, uint32_t *slot) {
    *slot = 0;
    if (!s)
        return 0;
    if (s->kind == K_D3D9SURFACE && s->front_obj) {
        ComObj *tex = com_get(s->front_obj);
        if (tex && tex->kind == K_D3D9TEXTURE) {
            for (size_t i = 0; i < tex->surfaces.size(); ++i)
                if (tex->surfaces[i] == s->id) {
                    *slot = (uint32_t)i;
                    return tex->id;
                }
        }
    }
    return s->id;
}
static HostD9Surface surface_ref(ComObj *s) {
    HostD9Surface r = {0, 0, 0};
    if (!s)
        return r;
    uint32_t slot;
    r.id = mirror_slot(s, &slot);
    ComObj *owner = com_get(r.id);
    if (owner && owner->kind == K_D3D9TEXTURE && owner->caps) {
        r.face = slot % 6;
        r.level = slot / 6;
    } else {
        r.level = slot;
    }
    return r;
}
static uint32_t slot_count(ComObj *o) {
    if (o->kind == K_D3D9TEXTURE)
        return (uint32_t)std::max<size_t>(o->surfaces.size(), 1);
    return 1;
}
static ComObj *slot_surface(ComObj *o, uint32_t slot) {
    if (o->kind != K_D3D9TEXTURE)
        return o;
    return slot < o->surfaces.size() ? com_get(o->surfaces[slot]) : nullptr;
}

// Creates the mirror of a texture or stand-alone surface on first use.
static GpuMirror &gpu_define(ComObj *o) {
    GpuMirror &m = gpu_mirrors()[o->id];
    if (m.defined)
        return m;
    HostD9TextureDesc d{};
    d.id = o->id;
    d.format = o->rmask ? o->rmask : 22;
    d.width = o->width ? o->width : 1;
    d.height = o->height ? o->height : 1;
    uint32_t usage = resource_usage()[o->id];
    if (is_depth_format(d.format))
        usage |= USAGE_DEPTHSTENCIL;
    d.usage = (usage & USAGE_RENDERTARGET ? HOST_D9_USAGE_RENDERTARGET : 0) |
              (usage & USAGE_DEPTHSTENCIL ? HOST_D9_USAGE_DEPTH : 0);
    d.samples = o->kind == K_D3D9SURFACE ? o->samples : 0;
    if (o->kind == K_D3D9TEXTURE && o->caps) {
        d.kind = HOST_D9_TEX_CUBE;
        d.levels = o->pal_flags ? o->pal_flags : 1;
    } else {
        d.kind = HOST_D9_TEX_2D;
        d.levels = o->kind == K_D3D9TEXTURE ? slot_count(o) : 1;
    }
    host_d9_texture_define(&d);
    m.defined = true;
    uint32_t n = o->kind == K_D3D9TEXTURE && o->caps ? 6 * d.levels : slot_count(o);
    // A target's first contents are whatever is drawn; a texture's are what
    // the game wrote, so every slot starts out needing an upload.
    m.cpu_dirty.assign(n, d.usage ? 0 : 1);
    m.gpu_newer.assign(n, 0);
    return m;
}

// Uploads every slot the game changed since the GPU last saw it.
static void gpu_sync(ComObj *o) {
    if (!o)
        return;
    GpuMirror &m = gpu_define(o);
    for (uint32_t slot = 0; slot < m.cpu_dirty.size(); ++slot) {
        if (!m.cpu_dirty[slot])
            continue;
        m.cpu_dirty[slot] = 0;
        ComObj *s = slot_surface(o, slot);
        if (!s || s->blob.empty())
            continue;
        HostD9Surface r = surface_ref(s);
        if (g_upload_trace_budget && recomp_env("BINK_TRACE")) {
            --g_upload_trace_budget;
            const uint8_t *b = resource_bytes(s);
            uint64_t hash = 1469598103934665603ull, nonzero = 0;
            for (size_t i = 0; i < s->blob.size(); ++i) {
                hash = (hash ^ b[i]) * 1099511628211ull;
                nonzero += b[i] != 0;
            }
            LOGW("d3d9: texture upload id=%u level=%u %ux%u pitch=%u bytes=%zu hash %016llx "
                 "nonzero %llu",
                 o->id, r.level, s->width, s->height, s->pitch, s->blob.size(),
                 (unsigned long long)hash, (unsigned long long)nonzero);
        }
        host_d9_texture_upload(o->id, r.face, r.level, resource_bytes(s), s->pitch);
    }
}

// The surface is about to be read or written by the game.
static void gpu_before_lock(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end() || slot >= it->second.gpu_newer.size() ||
        !it->second.gpu_newer[slot])
        return;
    it->second.gpu_newer[slot] = 0;
    if (s->bpp == 32 && !s->blob.empty()) {
        HostD9Surface r = surface_ref(s);
        host_d9_texture_read(id, r.face, r.level, s->blob.data(), s->pitch);
    }
}
// The game wrote the resource.
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi) {
    if (!o)
        return;
    if (o->kind == K_D3D9VB || o->kind == K_D3D9IB) {
        GpuMirror &m = gpu_mirrors()[o->id];
        if (m.dirty_hi <= m.dirty_lo) {
            m.dirty_lo = lo;
            m.dirty_hi = hi;
        } else {
            m.dirty_lo = std::min(m.dirty_lo, lo);
            m.dirty_hi = std::max(m.dirty_hi, hi);
        }
        m.defined = true;
        return;
    }
    uint32_t slot;
    uint32_t id = mirror_slot(o, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end())
        return; // not mirrored yet: its first use uploads everything
    if (slot < it->second.cpu_dirty.size())
        it->second.cpu_dirty[slot] = 1;
}
static void gpu_drawn_into(ComObj *s) {
    if (!s)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    ComObj *owner = com_get(id);
    if (!owner)
        return;
    GpuMirror &m = gpu_define(owner);
    if (slot < m.gpu_newer.size()) {
        m.gpu_newer[slot] = 1;
        m.cpu_dirty[slot] = 0;
    }
}
static void gpu_sync_buffer(ComObj *b) {
    if (!b)
        return;
    GpuMirror &m = gpu_mirrors()[b->id];
    if (!m.defined) {
        m.defined = true;
        m.dirty_lo = 0;
        m.dirty_hi = (uint32_t)b->blob.size();
    }
    if (m.dirty_hi > m.dirty_lo && !b->blob.empty()) {
        uint32_t hi = std::min<uint32_t>(m.dirty_hi, (uint32_t)b->blob.size());
        host_d9_buffer_upload(b->id, (uint32_t)b->blob.size(), m.dirty_lo,
                              b->blob.data() + m.dirty_lo, hi - m.dirty_lo);
    }
    m.dirty_lo = m.dirty_hi = 0;
}
static void gpu_forget(ComObj *o) {
    auto it = gpu_mirrors().find(o->id);
    if (it != gpu_mirrors().end()) {
        if (o->kind == K_D3D9VB || o->kind == K_D3D9IB)
            host_d9_buffer_drop(o->id);
        else if (it->second.defined)
            host_d9_texture_drop(o->id);
        gpu_mirrors().erase(it);
    }
    resource_usage().erase(o->id);
}

// (this, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle)
void Dev_CreateVertexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *vb = com_new(K_D3D9VB);
    if (!vb || !resource_alloc(vb, bytes)) {
        if (vb)
            com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    vb->dev_d3d = dev->id;
    vb->buf_bytes = bytes;
    uint32_t view_ = com_view(vb, IF_D3DVERTEXBUFFER9);
    if (!view_) {
        com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle)
void Dev_CreateIndexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *ib = com_new(K_D3D9IB);
    if (!ib || !resource_alloc(ib, bytes)) {
        if (ib)
            com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    ib->dev_d3d = dev->id;
    ib->buf_bytes = bytes;
    ib->rmask = arg(c, 3) == 102 ? 4 : 2; // D3DFMT_INDEX32 or D3DFMT_INDEX16
    uint32_t view_ = com_view(ib, IF_D3DINDEXBUFFER9);
    if (!view_) {
        com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
void Dev_CreateTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 5), out = arg(c, 7);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = w;
    tex->height = h;
    tex->bpp = format_bytes(fmt) * 8;
    // Every level gets its own surface. Levels = 0 asks for the full chain;
    // a game uploads each level with its own LockRect.
    uint32_t levels = arg(c, 3);
    if (!levels) {
        levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1)
            ++levels;
    }
    tex->rmask = fmt;
    uint32_t usage = arg(c, 4);
    resource_usage()[tex->id] = usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    for (uint32_t l = 0; l < levels && l < 16; ++l) {
        ComObj *s = make_surface(dev, std::max(w >> l, 1u), std::max(h >> l, 1u), fmt);
        if (!s)
            break;
        s->front_obj = tex->id;
        tex->surfaces.push_back(s->id);
    }
    tex->back_obj = tex->surfaces.empty() ? 0 : tex->surfaces[0];
    uint32_t view_ = com_view(tex, IF_D3DTEXTURE9);
    if (!view_) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
void Dev_CreateCubeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t edge = arg(c, 1), fmt = arg(c, 4), out = arg(c, 6);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = tex->height = edge;
    tex->bpp = format_bytes(fmt) * 8;
    tex->rmask = fmt;
    tex->caps = 1;                              // a cube map
    tex->pal_flags = arg(c, 2) ? arg(c, 2) : 1; // levels, reusing a field cubes do not use
    resource_usage()[tex->id] = arg(c, 3) & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    uint32_t view = com_view(tex, IF_D3DCUBETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    LOGV("d3d9: CreateCubeTexture %u -> %08x into %08x", edge, view, out);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static void create_surface_method(X86 *c, uint32_t out_index, uint32_t fmt_index, uint32_t usage) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, fmt_index), out = arg(c, out_index);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = make_surface(dev, w, h, fmt);
    if (!s) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    // CreateRenderTarget and CreateDepthStencilSurface: MultiSample, MultisampleQuality.
    if (out_index == 7)
        s->samples = multisample_count(arg(c, 4), arg(c, 5));
    resource_usage()[s->id] = usage;
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Discard,
//  ppSurface, pSharedHandle)
void Dev_CreateDepthStencilSurface(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_DEPTHSTENCIL);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
//  ppSurface, pSharedHandle)
void Dev_CreateRenderTarget(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_RENDERTARGET);
}
// (this, Width, Height, Format, Pool, ppSurface, pSharedHandle)
void Dev_CreateOffscreenPlainSurface(X86 *c) {
    create_surface_method(c, 5, 3, 0);
}

// The device's own back buffer and depth buffer, made on first ask and kept.
// The back buffer lives in its own field. It used to share render_target,
// which SetRenderTarget overwrites, so after the game switched targets the
// "back buffer" was whatever it had last drawn into.
static ComObj *device_backbuffer(ComObj *dev) {
    ComObj *s = com_get(dev->palette_obj);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 22);
        dev->palette_obj = s ? s->id : 0;
        if (s)
            s->samples = dev->samples;
        if (s)
            resource_usage()[s->id] = USAGE_RENDERTARGET;
        if (s && !com_get(dev->render_target))
            dev->render_target = s->id;
    }
    return s;
}

// (this, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter): a
// nearest-neighbour copy between 32-bit surfaces, rectangles honoured. This is
// how a game moves an off-screen target onto the back buffer.
void Dev_StretchRect(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 3));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        ComObj *sowner = com_get(mirror_slot(src, &slot));
        ComObj *downer = com_get(mirror_slot(dst, &slot));
        if (sowner)
            gpu_sync(sowner);
        if (downer)
            gpu_sync(downer);
        int32_t sr[4] = {0, 0, (int32_t)src->width, (int32_t)src->height};
        int32_t dr[4] = {0, 0, (int32_t)dst->width, (int32_t)dst->height};
        if (arg(c, 2))
            for (int i = 0; i < 4; ++i)
                sr[i] = (int32_t)rd32(arg(c, 2) + 4u * (uint32_t)i);
        if (arg(c, 4))
            for (int i = 0; i < 4; ++i)
                dr[i] = (int32_t)rd32(arg(c, 4) + 4u * (uint32_t)i);
        host_d9_stretch(surface_ref(src), sr, surface_ref(dst), dr, arg(c, 5));
        gpu_drawn_into(dst);
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *sbytes = resource_bytes(src), *dbytes = resource_bytes(dst);
    if (!sbytes || !dbytes || src->bpp != 32 || dst->bpp != 32) {
        com_ret(c, D3D_OK9);
        return;
    }
    auto rect = [](uint32_t p, ComObj *s, int32_t r[4]) {
        r[0] = 0;
        r[1] = 0;
        r[2] = (int32_t)s->width;
        r[3] = (int32_t)s->height;
        if (p)
            for (int i = 0; i < 4; ++i)
                r[i] = (int32_t)rd32(p + 4u * (uint32_t)i);
        r[0] = std::max(r[0], 0);
        r[1] = std::max(r[1], 0);
        r[2] = std::min(r[2], (int32_t)s->width);
        r[3] = std::min(r[3], (int32_t)s->height);
    };
    ComObj *dev_ = com_get(dst->dev_d3d);
    static uint32_t g_stretches = 0;
    if (++g_stretches <= 24)
        LOGW("d3d9: StretchRect %u (%ux%u, %u bpp) -> %u (%ux%u, %u bpp)%s", src->id, src->width,
             src->height, src->bpp, dst->id, dst->width, dst->height, dst->bpp,
             (dev_ && dst->id == dev_->palette_obj) ? ", onto the back buffer" : "");
    int32_t sr[4], dr[4];
    rect(arg(c, 2), src, sr);
    rect(arg(c, 4), dst, dr);
    int32_t sw = sr[2] - sr[0], sh = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        com_ret(c, D3D_OK9);
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = sr[1] + y * sh / dh;
        uint8_t *drow = dbytes + (size_t)(dr[1] + y) * dst->pitch;
        const uint8_t *srow = sbytes + (size_t)sy * src->pitch;
        for (int32_t x = 0; x < dw; ++x)
            memcpy(drow + 4 * (dr[0] + x), srow + 4 * (sr[0] + x * sw / dw), 4);
    }
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}
static ComObj *device_depthbuffer(ComObj *dev) {
    ComObj *s = com_get(dev->zbuffer_obj);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 75);
        dev->zbuffer_obj = s ? s->id : 0;
        if (s)
            s->samples = dev->samples;
        if (s)
            resource_usage()[s->id] = USAGE_DEPTHSTENCIL;
    }
    return s;
}

// Drops the device's own back and depth buffers; the game's references to them
// keep the objects alive, and the device makes new ones on next use.
static void device_forget_buffers(ComObj *dev) {
    for (uint32_t *field : {&dev->palette_obj, &dev->zbuffer_obj}) {
        ComObj *s = com_get(*field);
        if (s)
            com_release(s);
        *field = 0;
    }
}

// (this, iSwapChain, iBackBuffer, Type, ppBackBuffer)
void Dev_GetBackBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 4);
    ComObj *s = dev ? device_backbuffer(dev) : nullptr;
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, RenderTargetIndex, ppRenderTarget). Index 0 is the back buffer until
// SetRenderTarget replaces it. The caller AddRef balance is a later Release,
// so a stub that leaves the out pointer null is a null Release.
void Dev_GetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || !gm_valid(out, 4) || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *s = com_get(pl.color_target[index]);
    if (!s && index == 0)
        s = com_get(dev->render_target);
    if (!s && index == 0)
        s = device_backbuffer(dev);
    if (!s || s->kind != K_D3D9SURFACE) {
        wr32(out, 0);
        com_ret(c, MAKE_D3DHRESULT(2150)); // D3DERR_NOTFOUND
        return;
    }
    com_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    ComObj *s = dev ? device_depthbuffer(dev) : nullptr;
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 2));
    uint32_t index = arg(c, 1);
    if (!dev || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (index == 0 && !(s && s->kind == K_D3D9SURFACE)) {
        com_ret(c, D3DERR_INVALIDCALL); // target 0 cannot be unset
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    pl.color_target[index] = (s && s->kind == K_D3D9SURFACE) ? s->id : 0;
    if (index == 0) {
        dev->render_target = s->id;
        // Setting render target 0 resets the viewport and scissor to the target.
        pl.viewport_set = false;
        pl.scissor[0] = pl.scissor[1] = 0;
        pl.scissor[2] = (int32_t)s->width;
        pl.scissor[3] = (int32_t)s->height;
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 1));
    if (dev)
        dev->zbuffer_obj = (s && s->kind == K_D3D9SURFACE) ? s->id : 0;
    com_ret(c, D3D_OK9);
}

// (this, pVertexElements, ppDecl): the element array is kept verbatim, since
// only the eventual renderer can interpret it.
void Dev_CreateVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t elems = arg(c, 1), out = arg(c, 2);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_new(K_D3D9DECL);
    if (!d) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    d->dev_d3d = dev->id;
    // D3DVERTEXELEMENT9 is 8 bytes and the array ends with D3DDECL_END, whose
    // Stream is 0xff. Copy until that, so the declaration survives the call.
    for (uint32_t i = 0; i < 64 && elems; ++i) {
        uint32_t e = elems + i * 8u;
        uint16_t stream = rd16(e);
        for (int b = 0; b < 8; ++b)
            d->blob.push_back(gm_ptr(e)[b]);
        if (stream == 0xff)
            break;
    }
    uint32_t view_ = com_view(d, IF_D3DVERTEXDECL9);
    if (!view_) {
        com_release(d);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    if (d9_vertex_trace())
        LOGW("d3d9 vertex trace: CreateVertexDeclaration return=%08x input=%08x out=%08x object=%u view=%08x elements=%zu",
             rd32(c->r[R_ESP]), elems, out, d->id, view_, d->blob.size() / 8);
    com_ret(c, D3D_OK9);
}

void Dev_SetVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d = com_this(arg(c, 1));
    if (dev) {
        dev->current_viewport = (d && d->kind == K_D3D9DECL) ? d->id : 0;
        d9_pipeline(dev->id).declaration = dev->current_viewport;
    }
    if (d9_vertex_trace()) {
        static uint32_t traced = 0;
        if (traced++ < 64) {
        LOGW("d3d9 vertex trace: SetVertexDeclaration return=%08x arg=%08x object=%u",
             rd32(c->r[R_ESP]), arg(c, 1), d ? d->id : 0);
        d9_trace_declaration(d);
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, ppQuery). A query with no GPU behind it answers immediately.
void Dev_CreateQuery(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (!out) {
        com_ret(c, D3D_OK9); // a null out pointer only asks whether the type exists
        return;
    }
    ComObj *q = com_new(K_D3D9QUERY);
    if (!q) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    q->dev_d3d = dev ? dev->id : 0;
    q->dev_type = type;
    uint32_t view_ = com_view(q, IF_D3DQUERY9);
    if (!view_) {
        com_release(q);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, pStreamData, OffsetInBytes, Stride)
void Dev_SetStreamSource(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *vb = com_this(arg(c, 2));
    uint32_t stream = arg(c, 1);
    if (dev && stream < 8) {
        D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
        st.vb = (vb && vb->kind == K_D3D9VB) ? vb->id : 0;
        st.offset = arg(c, 3);
        st.stride = arg(c, 4);
    }
    if (dev && stream == 0 && vb && vb->kind == K_D3D9VB) {
        dev->back_obj = vb->id;
        dev->lock_off = arg(c, 3);
        dev->rate = arg(c, 4); // the stride, reusing a spare field
    }
    if (d9_vertex_trace()) {
        static uint32_t traced = 0;
        if (traced++ < 64)
        LOGW("d3d9 vertex trace: SetStreamSource return=%08x stream=%u arg=%08x object=%u offset=%u stride=%u",
             rd32(c->r[R_ESP]), stream, arg(c, 2), vb ? vb->id : 0, arg(c, 3), arg(c, 4));
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetIndices(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *ib = com_this(arg(c, 1));
    if (dev) {
        dev->front_obj = (ib && ib->kind == K_D3D9IB) ? ib->id : 0;
        d9_pipeline(dev->id).index_buffer = dev->front_obj;
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, pTexture): the texture behind a sampler, for the renderer.
void Dev_SetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *tex = com_this(arg(c, 2));
    uint32_t stage = arg(c, 1);
    if (dev) {
        dev->texture_handle = tex ? tex->id : 0;
        if (stage < 16)
            d9_pipeline(dev->id).sampler_tex[stage] = tex ? tex->id : 0;
    }
    com_ret(c, D3D_OK9);
}

// The draws. Nothing is rasterized yet, so each one reports what it was asked
// to draw: the geometry a renderer would need is all in these arguments, and
// a run that names them is the specification for the work that comes next.
// The first few are reported in full and then every 256th, because a game at
// speed issues thousands.
static uint32_t g_draws = 0;

static const char *prim_name(uint32_t type) {
    switch (type) {
    case 1:
        return "point list";
    case 2:
        return "line list";
    case 3:
        return "line strip";
    case 4:
        return "triangle list";
    case 5:
        return "triangle strip";
    case 6:
        return "triangle fan";
    default:
        return "unknown primitive";
    }
}

// The first few draws, then (with verbose logging) every 256th: a race makes
// thousands a frame, and writing even one in 256 cost a percent of the frame.
static bool draw_worth_reporting() {
    ++g_draws;
    return g_draws <= 8 || ((g_draws % 256) == 0 && log_level() >= 2);
}

static uint32_t primitive_vertices(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1:
        return count; // points
    case 2:
        return count * 2; // lines
    case 3:
        return count + 1; // line strip
    case 4:
        return count * 3; // triangles
    case 5:
    case 6:
        return count + 2;
    default:
        return 0;
    }
}

static void d9_trace_stream_draw(X86 *c, ComObj *dev) {
    if (!d9_vertex_trace() || !dev || g_draws > 8)
        return;
    D9Pipeline &pl = d9_pipeline(dev->id);
    LOGW("d3d9 vertex trace: draw=%u return=%08x primitive=%u start=%u count=%u fvf=%08x decl=%u ps=%u(%zu bytes) vs=%s",
         g_draws, rd32(c->r[R_ESP]), arg(c, 1), arg(c, 2), arg(c, 3), pl.fvf,
         pl.declaration, pl.pixel_shader, pl.ps.size(), pl.vs.empty() ? "empty" : "bound");
    d9_trace_declaration(com_get(pl.declaration));
    for (uint32_t stream = 0; stream < 8; ++stream) {
        const D9Pipeline::Stream &state = pl.stream[stream];
        if (!state.vb && !state.stride)
            continue;
        ComObj *vb = com_get(state.vb);
        LOGW("d3d9 vertex trace: draw=%u stream=%u object=%u offset=%u stride=%u buffer_bytes=%zu",
             g_draws, stream, state.vb, state.offset, state.stride,
             vb ? vb->blob.size() : 0);
        if (!vb || !resource_bytes(vb) || !state.stride)
            continue;
        uint32_t count = primitive_vertices(arg(c, 1), arg(c, 3));
        for (uint32_t v = 0; v < count && v < 16; ++v) {
            uint64_t at = (uint64_t)state.offset + (uint64_t)(arg(c, 2) + v) * state.stride;
            if (at > vb->blob.size() || state.stride > vb->blob.size() - at)
                break;
            static const char hex[] = "0123456789abcdef";
            std::string bytes;
            const uint8_t *src = resource_bytes(vb) + at;
            for (uint32_t i = 0; i < state.stride && i < 128; ++i) {
                bytes.push_back(hex[src[i] >> 4]);
                bytes.push_back(hex[src[i] & 15]);
            }
            LOGW("d3d9 vertex trace: draw=%u stream=%u vertex=%u raw=%s%s", g_draws,
                 stream, v, bytes.c_str(), state.stride > 128 ? " (truncated)" : "");
        }
    }
}

// The render target and depth buffer a draw or clear goes to, mirrored.
static HostD9Target gpu_target(ComObj *dev) {
    HostD9Target t{};
    D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (i == 0 && !s)
            s = com_get(dev->render_target) ? com_get(dev->render_target) : device_backbuffer(dev);
        if (!s)
            continue;
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(s, &slot));
        if (owner)
            gpu_sync(owner);
        t.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj)) {
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(z, &slot));
        if (owner)
            gpu_sync(owner);
        t.depth = surface_ref(z);
    }
    return t;
}
static void gpu_target_drawn(ComObj *dev, bool color, bool depth) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (color)
        for (int i = 0; i < 4; ++i) {
            ComObj *s = com_get(pl.color_target[i]);
            if (i == 0 && !s)
                s = com_get(dev->render_target);
            gpu_drawn_into(s);
        }
    if (depth)
        gpu_drawn_into(com_get(dev->zbuffer_obj));
}
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.viewport_set) {
        for (int i = 0; i < 4; ++i)
            vp[i] = (int32_t)pl.viewport[i];
        return;
    }
    ComObj *rt =
        com_get(pl.color_target[0]) ? com_get(pl.color_target[0]) : com_get(dev->render_target);
    (void)t;
    vp[0] = vp[1] = 0;
    vp[2] = rt ? (int32_t)rt->width : (int32_t)dev->width;
    vp[3] = rt ? (int32_t)rt->height : (int32_t)dev->height;
}

// One draw, handed to the GPU renderer with everything the device has bound.
static void gpu_draw(ComObj *dev, HostD9Draw &d) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.vs.empty() || pl.ps.empty()) {
        log_once("d3d9.gpu.ffp",
                 "d3d9: draw skipped: vs %s (SetVertexShader %s %08x), ps %s, fvf=%08x, decl=%u",
                 pl.vs.empty() ? "empty" : "bound", pl.vertex_shader_set ? "called" : "not-called",
                 pl.vertex_shader_arg, pl.ps.empty() ? "empty" : "bound", pl.fvf, pl.declaration);
        return;
    }
    d.target = gpu_target(dev);
    target_viewport(dev, d.target, d.viewport);
    d.depth_range[0] = pl.viewport_z[0];
    d.depth_range[1] = pl.viewport_z[1];
    for (int i = 0; i < 4; ++i)
        d.scissor[i] = pl.scissor[i];
    if (!pl.vs_key)
        pl.vs_key = d9sh::code_key(pl.vs.data(), pl.vs.size());
    if (!pl.ps_key)
        pl.ps_key = d9sh::code_key(pl.ps.data(), pl.ps.size());
    const d9sh::Program &vs = d9sh::program_for_key(pl.vs_key, pl.vs.data(), pl.vs.size());
    const d9sh::Program &ps = d9sh::program_for_key(pl.ps_key, pl.ps.data(), pl.ps.size());
    d.vs_key = pl.vs_key;
    d.ps_key = pl.ps_key;
    d.vs = pl.vs.data();
    d.vs_size = (uint32_t)pl.vs.size();
    d.ps = pl.ps.data();
    d.ps_size = (uint32_t)pl.ps.size();
    d.vconst = &pl.vconst[0][0];
    d.vconst_count = std::min<uint32_t>(vs.max_const, 256);
    d.pconst = &pl.pconst[0][0];
    d.pconst_count = std::min<uint32_t>(ps.max_const, 32);
    ComObj *decl = com_get(pl.declaration);
    if (!decl || decl->kind != K_D3D9DECL) {
        log_once("d3d9.gpu.nodecl",
                 "d3d9: a draw with no vertex declaration is skipped (fvf=%08x)", pl.fvf);
        return;
    }
    d.decl = decl->blob.data();
    d.decl_size = (uint32_t)decl->blob.size();
    d.decl_id = decl->id;
    for (int i = 0; i < 8; ++i) {
        if (d.stream[i].stride && !d.stream[i].buffer)
            continue; // the draw's own inline vertices
        ComObj *vb = com_get(pl.stream[i].vb);
        if (!vb)
            continue;
        gpu_sync_buffer(vb);
        d.stream[i].buffer = vb->id;
        d.stream[i].offset = pl.stream[i].offset;
        d.stream[i].stride = pl.stream[i].stride;
    }
    for (int s = 0; s < 16; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex || (tex->kind != K_D3D9TEXTURE && tex->kind != K_D3D9SURFACE))
            continue;
        gpu_sync(tex);
        d.sampler_texture[s] = tex->id;
    }
    if (d3d_color_trace()) {
        constexpr uint64_t face_vs = 0x2795ecec36a3088full;
        constexpr uint64_t curtain_vs = 0xf1a9ea695a102a16ull;
        static uint32_t face_seen = 0, curtain_seen = 0;
        ComObj *tex0 = com_get(pl.sampler_tex[0]);
        uint32_t *seen = pl.vs_key == face_vs && tex0 && tex0->width == 128 &&
                                 tex0->height == 128 ? &face_seen
                         : pl.vs_key == curtain_vs ? &curtain_seen : nullptr;
        if (seen && (*seen)++ < (seen == &face_seen ? 4u : 2u)) {
            LOGW("d3d9 color trace: draw=%u vs=%016llx ps=%016llx tex=%u decl=%u prim=%u count=%u start=%u base=%d ib=%u index_size=%u blend=%u/%u/%u fog=%u",
                 g_draws, (unsigned long long)pl.vs_key, (unsigned long long)pl.ps_key,
                 d.sampler_texture[0], pl.declaration, d.primitive, d.primitive_count, d.start,
                 d.base_vertex, d.index_buffer, d.index_size, pl.rs[27], pl.rs[19], pl.rs[20],
                 pl.rs[28]);
            d9_trace_declaration(decl);
            for (uint32_t reg = 0; reg < 29; ++reg)
                LOGW("d3d9 color trace: draw=%u c%u=%.9g,%.9g,%.9g,%.9g", g_draws, reg,
                     pl.vconst[reg][0], pl.vconst[reg][1], pl.vconst[reg][2],
                     pl.vconst[reg][3]);
            for (uint32_t stream = 0; stream < 8; ++stream) {
                const auto &st = pl.stream[stream];
                ComObj *vb = com_get(st.vb);
                if (!vb || !st.stride)
                    continue;
                LOGW("d3d9 color trace: draw=%u stream=%u vb=%u bytes=%zu offset=%u stride=%u",
                     g_draws, stream, st.vb, vb->blob.size(), st.offset, st.stride);
                const uint8_t *data = resource_bytes(vb);
                ComObj *ib = com_get(d.index_buffer);
                const uint8_t *indices = resource_bytes(ib);
                for (uint32_t n = 0; n < 3 && data; ++n) {
                    uint32_t vertex = d.start + n;
                    if (indices && (d.index_size == 2 || d.index_size == 4) &&
                        (uint64_t)vertex * d.index_size + d.index_size <= ib->blob.size()) {
                        const uint8_t *p = indices + (uint64_t)vertex * d.index_size;
                        vertex = d.index_size == 2 ? uint32_t(p[0] | p[1] << 8)
                                                   : uint32_t(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24);
                    }
                    int64_t at = int64_t(st.offset) + (int64_t(vertex) + d.base_vertex) * st.stride;
                    if (at < 0 || uint64_t(at) + st.stride > vb->blob.size())
                        continue;
                    static const char hex[] = "0123456789abcdef";
                    std::string bytes;
                    for (uint32_t k = 0; k < st.stride && k < 64; ++k) {
                        uint8_t v = data[at + k];
                        bytes.push_back(hex[v >> 4]);
                        bytes.push_back(hex[v & 15]);
                    }
                    LOGW("d3d9 color trace: draw=%u stream=%u sample=%u vertex=%u raw=%s%s",
                         g_draws, stream, n, vertex, bytes.c_str(), st.stride > 64 ? "..." : "");
                }
            }
        }
    }
    if (d3d_rs_trace() &&
        ((pl.rs[27] && (!pl.rs[19] || !pl.rs[20])) || pl.rs[7] > 2 ||
         pl.rs[14] > 1 || pl.rs[15] > 1 || pl.rs[27] > 1 || pl.rs[28] > 1)) {
        LOGW("d3d9 rs trace: DRAW draw=%u device=%u version=%llu tex0=%u vs=%016llx ps=%016llx z=%08x/%08x atest=%08x blend=%08x src=%08x dst=%08x fog=%08x",
             g_draws, dev->id, (unsigned long long)pl.state_version, d.sampler_texture[0],
             (unsigned long long)pl.vs_key, (unsigned long long)pl.ps_key, pl.rs[7],
             pl.rs[14], pl.rs[15], pl.rs[27], pl.rs[19], pl.rs[20], pl.rs[28]);
    }
    d.sampler_state = &pl.sampler_state[0][0];
    d.render_state = pl.rs;
    d.render_state_set = (const uint8_t *)pl.rs_set;
    d.state_version = pl.state_version;
    for (int st = 0; st < 8; ++st)
        if (pl.tss[st][24] & 0x100) // D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED
            d.projected_mask |= 1u << st;
    d.label = pl.label;
    static bool readback_attempted = false;
    std::vector<uint8_t> before;
    uint32_t read_id = 0, read_face = 0, read_level = 0, read_pitch = 0;
    bool before_ok = false;
    if (d9_vertex_trace() && !readback_attempted) {
        readback_attempted = true;
        HostD9Surface color = d.target.color[0];
        ComObj *owner = com_get(color.id);
        LOGW("d3d9 vertex trace: first eligible draw target=%u face=%u level=%u size=%ux%u bpp=%u viewport=%d,%d %dx%d tex0=%u",
             color.id, color.face, color.level, owner ? owner->width : 0,
             owner ? owner->height : 0, owner ? owner->bpp : 0, d.viewport[0], d.viewport[1],
             d.viewport[2], d.viewport[3], d.sampler_texture[0]);
        LOGW("d3d9 vertex trace: draw state z=%u zwrite=%u zfunc=%u cull=%u blend=%u colorwrite=%x alpha=%u/%u fog=%u scissor=%u rect=%d,%d,%d,%d",
             pl.rs[7], pl.rs[14], pl.rs[23], pl.rs[22], pl.rs[27], pl.rs[168],
             pl.rs[15], pl.rs[25], pl.rs[28], pl.rs[174], d.scissor[0], d.scissor[1],
             d.scissor[2], d.scissor[3]);
        for (uint32_t reg : {0u, 14u, 15u, 16u, 17u, 49u, 55u})
            LOGW("d3d9 vertex trace: vertex constant c%u = %.9g %.9g %.9g %.9g",
                 reg, pl.vconst[reg][0], pl.vconst[reg][1], pl.vconst[reg][2],
                 pl.vconst[reg][3]);
        ComObj *sample = com_get(pl.sampler_tex[0]);
        if (sample)
            LOGW("d3d9 vertex trace: texture0 object=%u kind=%u size=%ux%u bpp=%u slots=%zu",
                 sample->id, (unsigned)sample->kind, sample->width, sample->height, sample->bpp,
                 sample->surfaces.size());
        if (owner && owner->bpp == 32 && owner->width && owner->height && owner->width <= 4096 &&
            owner->height <= 4096) {
            read_id = color.id;
            read_face = color.face;
            read_level = color.level;
            read_pitch = owner->width * 4;
            before.resize((size_t)read_pitch * owner->height);
            before_ok = host_d9_texture_read(read_id, read_face, read_level, before.data(), read_pitch);
        }
    }
    host_d9_draw(&d);
    if (d9_vertex_trace() && read_id) {
        std::vector<uint8_t> after(before.size());
        bool after_ok = host_d9_texture_read(read_id, read_face, read_level, after.data(), read_pitch);
        size_t changed = 0, nonzero = 0;
        if (before_ok && after_ok)
            for (size_t i = 0; i < after.size(); ++i) {
                changed += before[i] != after[i];
                nonzero += after[i] != 0;
            }
        LOGW("d3d9 vertex trace: first eligible draw readback before=%d after=%d changed_bytes=%zu nonzero_bytes=%zu",
             before_ok ? 1 : 0, after_ok ? 1 : 0, changed, nonzero);
    }
    bool color_write = !pl.rs_set[168] || (pl.rs[168] & 0xf) != 0;
    bool z_write = (!pl.rs_set[14] || pl.rs[14]) && (!pl.rs_set[7] || pl.rs[7]);
    gpu_target_drawn(dev, color_write, z_write);
}

// Hands one draw to the CPU renderer with what the device has bound.
static void raster(X86 *c, const D9DrawCall &call) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *target = com_get(dev->render_target);
    if (!target)
        target = device_backbuffer(dev);
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    static const std::vector<uint8_t> none;
    d9_raster_draw(dev, target, (decl && decl->kind == K_D3D9DECL) ? decl->blob : none, call);
}

// (this, PrimitiveType, StartVertex, PrimitiveCount)
void Dev_DrawPrimitive(X86 *c) {
    if (draw_worth_reporting()) {
        D9Pipeline *pl = nullptr;
        if (ComObj *dev = this_device9(c))
            pl = &d9_pipeline(dev->id);
        LOGW("d3d9: draw %u: %s, %u primitives from vertex %u, from the bound stream "
             "fvf=%08x decl=%u ps=%s (%zu bytes) vs=%s",
             g_draws, prim_name(arg(c, 1)), arg(c, 3), arg(c, 2), pl ? pl->fvf : 0,
             pl ? pl->declaration : 0, pl && !pl->ps.empty() ? "bound" : "empty",
             pl ? pl->ps.size() : 0, pl && !pl->vs.empty() ? "bound" : "empty");
    }
    ComObj *dev = this_device9(c);
    d9_trace_stream_draw(c, dev);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 3);
        d.start = arg(c, 2);
        gpu_draw(dev, d);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    if (vb && resource_bytes(vb) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 3);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.first = arg(c, 2);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
void Dev_DrawIndexedPrimitive(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices from index %u, indexed", g_draws,
             prim_name(arg(c, 1)), arg(c, 6), arg(c, 4), arg(c, 5));
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        ComObj *ibo = com_get(d9_pipeline(dev->id).index_buffer);
        if (ibo) {
            gpu_sync_buffer(ibo);
            HostD9Draw d{};
            d.primitive = arg(c, 1);
            d.primitive_count = arg(c, 6);
            d.start = arg(c, 5);
            d.base_vertex = (int32_t)arg(c, 2);
            d.index_buffer = ibo->id;
            d.index_size = ibo->rmask ? ibo->rmask : 2;
            gpu_draw(dev, d);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    ComObj *ib = dev ? com_get(dev->front_obj) : nullptr;
    if (vb && resource_bytes(vb) && ib && resource_bytes(ib) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 6);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.index_data = resource_bytes(ib);
        call.index_bytes = ib->blob.size();
        call.index_size = ib->rmask ? ib->rmask : 2;
        call.base_vertex = (int32_t)arg(c, 2);
        call.first = arg(c, 5);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride): the
// vertices travel with the call rather than in a buffer, which is how this
// game draws.
// What a rasterizer will need from a draw, spelled out for the first few: the
// bound vertex declaration decoded, the raw vertices as floats, the bound
// texture, and the surface being drawn into.
static void describe_draw_inputs(X86 *c, uint32_t vertices, uint32_t stride, uint32_t nverts) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    D9Pipeline &pl = d9_pipeline(dev->id);
    LOGW("d3d9:   fvf=%08x vs_set=%d vs_arg=%08x ps=%s ps_obj=%u decl=%u", pl.fvf,
         pl.vertex_shader_set ? 1 : 0, pl.vertex_shader_arg, pl.ps.empty() ? "empty" : "bound",
         pl.pixel_shader, pl.declaration);
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    if (decl && decl->kind == K_D3D9DECL) {
        static const char *types[] = {"FLOAT1",    "FLOAT2",   "FLOAT3",   "FLOAT4",  "D3DCOLOR",
                                      "UBYTE4",    "SHORT2",   "SHORT4",   "UBYTE4N", "SHORT2N",
                                      "SHORT4N",   "USHORT2N", "USHORT4N", "UDEC3",   "DEC3N",
                                      "FLOAT16_2", "FLOAT16_4"};
        static const char *usages[] = {
            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE"};
        char line[512];
        size_t used = 0;
        for (size_t i = 0; i + 8 <= decl->blob.size(); i += 8) {
            uint16_t stream = (uint16_t)(decl->blob[i] | (decl->blob[i + 1] << 8));
            if (stream == 0xff)
                break;
            uint16_t off = (uint16_t)(decl->blob[i + 2] | (decl->blob[i + 3] << 8));
            uint8_t type = decl->blob[i + 4], usage = decl->blob[i + 6], index = decl->blob[i + 7];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s%u@%u:%s",
                                     usage < 14 ? usages[usage] : "?", index, off,
                                     type < 17 ? types[type] : "?");
            if (used >= sizeof line)
                break;
        }
        LOGW("d3d9:   declaration %u:%s", decl->id, line);
    } else {
        LOGW("d3d9:   no vertex declaration bound (FVF path)");
    }
    for (uint32_t v = 0; v < nverts && v < 4; ++v) {
        uint32_t base = vertices + v * stride;
        char line[256];
        size_t used = 0;
        for (uint32_t o = 0; o + 4 <= stride && o < 40; o += 4)
            used += (size_t)snprintf(
                line + used, sizeof line - used, " %08x:%g", [&] {
                    uint32_t bits = rd32(base + o);
                    return bits;
                }(),
                (double)[&] {
                    float f;
                    uint32_t bits = rd32(base + o);
                    memcpy(&f, &bits, 4);
                    return f;
                }());
        LOGW("d3d9:   vertex %u:%s", v, line);
    }
    ComObj *tex = com_get(dev->texture_handle);
    ComObj *rt = com_get(dev->render_target);
    LOGW("d3d9:   texture %u (%ux%u), target %u (%ux%u)%s", tex ? tex->id : 0, tex ? tex->width : 0,
         tex ? tex->height : 0, rt ? rt->id : 0, rt ? rt->width : 0, rt ? rt->height : 0,
         (rt && rt->id == dev->palette_obj) ? ", the back buffer" : "");
}

void Dev_DrawPrimitiveUP(X86 *c) {
    if (draw_worth_reporting()) {
        LOGW("d3d9: draw %u: %s, %u primitives, %u-byte vertices inline at %08x", g_draws,
             prim_name(arg(c, 1)), arg(c, 2), arg(c, 4), arg(c, 3));
        // A fan of n primitives has n + 2 vertices.
        if (g_draws <= 8)
            describe_draw_inputs(c, arg(c, 3), arg(c, 4), arg(c, 2) + 2);
        if (g_draws == 1) {
            if (ComObj *dev = this_device9(c)) {
                D9Pipeline &pl = d9_pipeline(dev->id);
                auto row = [](const float *m) {
                    char b[192];
                    snprintf(b, sizeof b,
                             "%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", m[0], m[1],
                             m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                             m[13], m[14], m[15]);
                    return std::string(b);
                };
                LOGW("d3d9:   viewport set=%d %u,%u %ux%u z=%g..%g", pl.viewport_set, pl.viewport[0],
                     pl.viewport[1], pl.viewport[2], pl.viewport[3], pl.viewport_z[0],
                     pl.viewport_z[1]);
                LOGW("d3d9:   view %s", row(pl.transform[2]).c_str());
                LOGW("d3d9:   proj %s", row(pl.transform[3]).c_str());
                LOGW("d3d9:   world %s", row(pl.transform[24]).c_str());
                LOGW("d3d9:   rs ZENABLE=%u set=%d ALPHABLEND=%u set=%d CULL=%u set=%d "
                     "TEXTUREFACTOR=%08x set=%d LIGHTING=%u set=%d COLORVERTEX=%u set=%d "
                     "DIFFUSEMATERIALSOURCE=%u set=%d SRCBLEND=%u DESTBLEND=%u",
                     pl.rs[7], pl.rs_set[7], pl.rs[27], pl.rs_set[27], pl.rs[22], pl.rs_set[22],
                     pl.rs[60], pl.rs_set[60], pl.rs[137], pl.rs_set[137], pl.rs[141],
                     pl.rs_set[141], pl.rs[145], pl.rs_set[145], pl.rs[19], pl.rs[20]);
                LOGW("d3d9:   tss0 COLOROP=%u COLORARG1=%u COLORARG2=%u ALPHAOP=%u sampler0=%u",
                     pl.tss[0][1], pl.tss[0][2], pl.tss[0][3], pl.tss[0][4], pl.sampler_tex[0]);
            }
        }
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 2);
        uint32_t bytes = primitive_vertices(d.primitive, d.primitive_count) * arg(c, 4);
        if (bytes && gm_fits(arg(c, 3), bytes)) {
            d.stream[0].stride = arg(c, 4);
            d.inline_vertices = gm_ptr(arg(c, 3));
            d.inline_bytes = bytes;
            gpu_draw(dev, d);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 2);
    call.vertices = arg(c, 3);
    call.stride = arg(c, 4);
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
//  pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride)
void Dev_DrawIndexedPrimitiveUP(X86 *c) {
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices of %u bytes inline at %08x, "
             "indices at %08x",
             g_draws, prim_name(arg(c, 1)), arg(c, 4), arg(c, 3), arg(c, 8), arg(c, 7), arg(c, 5));
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 4);
        uint32_t isize = arg(c, 6) == 102 ? 4 : 2;
        uint32_t ibytes = primitive_vertices(d.primitive, d.primitive_count) * isize;
        uint32_t vbytes = (arg(c, 2) + arg(c, 3)) * arg(c, 8);
        if (ibytes && vbytes && gm_fits(arg(c, 5), ibytes) && gm_fits(arg(c, 7), vbytes)) {
            d.stream[0].stride = arg(c, 8);
            d.inline_vertices = gm_ptr(arg(c, 7));
            d.inline_bytes = vbytes;
            d.inline_indices = gm_ptr(arg(c, 5));
            d.inline_index_bytes = ibytes;
            d.index_size = isize;
            gpu_draw(dev, d);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 4);
    call.vertices = arg(c, 7);
    call.stride = arg(c, 8);
    call.indices = arg(c, 5);
    call.index_size = arg(c, 6) == 102 ? 4 : 2;
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// The resource interfaces themselves
// ---------------------------------------------------------------------------
#define RES_STUB(iface, name)                                                                      \
    void iface##_##name(X86 *c) {                                                                  \
        log_once("d3d9." #iface "." #name, "d3d9: " #iface "::" #name " is not implemented");      \
        com_ret(c, D3D_OK9);                                                                       \
    }

RES_STUB(Res, SetPrivateData)
RES_STUB(Res, GetPrivateData)
RES_STUB(Res, FreePrivateData)
RES_STUB(Res, SetPriority)
RES_STUB(Res, GetPriority)
RES_STUB(Res, PreLoad)
RES_STUB(Res, GetType)
RES_STUB(Tex, SetLOD)
RES_STUB(Tex, GetLOD)
RES_STUB(Tex, SetAutoGenFilterType)
RES_STUB(Tex, GetAutoGenFilterType)
RES_STUB(Tex, GenerateMipSubLevels)
RES_STUB(Tex, AddDirtyRect)
RES_STUB(Tex, AddDirtyRectCube)
RES_STUB(Surf, GetDC)
RES_STUB(Surf, ReleaseDC)
RES_STUB(Decl, GetDeclaration)

// (this, riid, ppContainer): the texture a level or face belongs to, or the
// device when the surface stands alone.
void Surf_GetContainer(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *parent = com_get(s->front_obj);
    if (parent) {
        com_addref(parent);
        com_out_ptr(out, com_view(parent, parent->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9));
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *dev = com_get(s->dev_d3d);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

void Res_GetDevice(X86 *c) {
    ComObj *o = com_this_arg(c);
    ComObj *dev = o ? com_get(o->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

// Lock hands the guest the storage this resource was created with. Unlock is
// a no-op: there is nothing to upload to yet.
static void buffer_lock(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset = arg(c, 1), out = arg(c, 3);
    if (!o || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = offset < o->blob.size() ? stage_lock(o) : 0;
    com_out_ptr(out, staged ? staged + offset : 0);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void VB_Lock(X86 *c) {
    buffer_lock(c);
}
void IB_Lock(X86 *c) {
    buffer_lock(c);
}
void Buf_Unlock(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size.
void Buf_GetDesc(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!o || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 20);
    wr32(d + 16, o->buf_bytes);
    com_ret(c, D3D_OK9);
}

void d3d9_trace_locks(unsigned count) {
    g_lock_trace_budget = count;
    g_upload_trace_budget = count;
}
static bool trace_lock() {
    if (!g_lock_trace_budget || !recomp_env("BINK_TRACE"))
        return false;
    --g_lock_trace_budget;
    return true;
}
// D3DLOCKED_RECT is { INT Pitch; void *pBits; }.
void Surf_LockRect(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 1);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    if (trace_lock())
        LOGW("d3d9: Surf_LockRect return=%08x surf=%u owner=%u %ux%u -> pBits=%08x pitch=%u",
             rd32(c->r[R_ESP]), s->id, s->front_obj, s->width, s->height, staged, s->pitch);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Surf_UnlockRect(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality,
// Width, Height.
void Surf_GetDesc(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!s || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, s->rmask ? s->rmask : 22);
    wr32(d + 24, s->width);
    wr32(d + 28, s->height);
    com_ret(c, D3D_OK9);
}

// A texture level is the surface made with the texture.
// A cube face, made on first ask and kept, so the game gets the same surface
// back every time. Returning success without writing the pointer - which is
// what the stub did - had the game store nothing and throw the cube away.
// The surface for one face and level of a cube, made on first ask and kept on
// the texture, indexed face + 6 * level.
static ComObj *cube_face(ComObj *tex, uint32_t face, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || !tex->caps || face > 5 || level > 15)
        return nullptr;
    size_t slot = face + 6u * level;
    if (tex->surfaces.size() <= slot)
        tex->surfaces.resize(slot + 1, 0);
    ComObj *s = com_get(tex->surfaces[slot]);
    if (!s) {
        uint32_t edge = std::max(tex->width >> level, 1u);
        s = make_surface(com_get(tex->dev_d3d), edge, edge, tex->rmask ? tex->rmask : 22);
        if (!s)
            return nullptr;
        s->front_obj = tex->id;
        tex->surfaces[slot] = s->id;
    }
    return s;
}

// (this, FaceType, Level, ppCubeMapSurface). A face is a view into the
// texture, so handing one out references the texture too: the game takes
// face zero, releases the cube, and goes on using it for the other five.
void Tex_GetCubeMapSurface(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 3);
    ComObj *s = cube_face(tex, arg(c, 1), arg(c, 2));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(s, IF_D3DSURFACE9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_addref(tex);
    com_addref(s);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

// Returns a count, not an HRESULT. The stub returned zero, which told the
// game its textures had no levels at all.
// (this, FaceType, Level, pLockedRect, pRect, Flags)
void Tex_LockRectCube(X86 *c) {
    ComObj *s = cube_face(com_this_arg(c), arg(c, 1), arg(c, 2));
    uint32_t out = arg(c, 3);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
// (this, FaceType, Level)
void Tex_UnlockRectCube(X86 *c) {
    stage_unlock(cube_face(com_this_arg(c), arg(c, 1), arg(c, 2)));
    com_ret(c, D3D_OK9);
}

void Tex_GetLevelCount(X86 *c) {
    ComObj *tex = com_this_arg(c);
    set_eax(c, (tex && !tex->caps && !tex->surfaces.empty()) ? (uint32_t)tex->surfaces.size() : 1);
}

// (this, Level, pDesc): the same D3DSURFACE_DESC a surface describes itself
// with, for the level the texture was created at.
void Tex_GetLevelDesc(X86 *c) {
    ComObj *tex = com_this_arg(c);
    ComObj *s = texture_level(tex, arg(c, 1));
    uint32_t d = arg(c, 2);
    if (!tex || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 32);
    wr32(d + 0, tex->rmask ? tex->rmask : 22);
    wr32(d + 24, s ? s->width : tex->width);
    wr32(d + 28, s ? s->height : tex->height);
    com_ret(c, D3D_OK9);
}

void Tex_GetSurfaceLevel(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 2);
    ComObj *s = texture_level(tex, arg(c, 1));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // A level is a view into the texture and references it, the same way a
    // cube face does.
    s->front_obj = tex->id;
    com_addref(tex);
    com_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Tex_LockRect(X86 *c) {
    ComObj *tex = com_this_arg(c);
    ComObj *s = texture_level(tex, arg(c, 1));
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    wr32(out, s->pitch);
    wr32(out + 4, staged);
    if (trace_lock())
        LOGW("d3d9: Tex_LockRect return=%08x tex=%u level=%u %ux%u fmt=%u usage=%08x rect=%08x "
             "flags=%08x -> pBits=%08x pitch=%u bytes=%zu",
             rd32(c->r[R_ESP]), tex ? tex->id : 0, arg(c, 1), s->width, s->height,
             s->rmask ? s->rmask : 22, tex ? resource_usage()[tex->id] : 0, arg(c, 3),
             arg(c, 4), staged, s->pitch, s->blob.size());
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Tex_UnlockRect(X86 *c) {
    stage_unlock(texture_level(com_this_arg(c), arg(c, 1)));
    com_ret(c, D3D_OK9);
}

// D3DQUERYTYPE: 8 is EVENT, 9 OCCLUSION. An occlusion query with a GPU
// renderer counts real samples; without one, or for any other type, the
// query is finished at once, and an occlusion count is reported as one
// sample so that nothing the game gates on visibility disappears.
static const uint32_t D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9;
static const uint32_t S_FALSE9 = 1;
static std::unordered_map<uint32_t, uint32_t> &query_state() { // id -> 1 begun, 2 ended
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static void query_destroy(ComObj *o) {
    if (query_state().erase(o->id))
        host_d9_query_drop(o->id);
}
void Query_GetType9(X86 *c) {
    ComObj *q = com_this_arg(c);
    set_eax(c, q ? q->dev_type : 0);
}
void Query_GetDataSize(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t type = q ? q->dev_type : 0;
    set_eax(c, (type == D3DQUERYTYPE_OCCLUSION || type == D3DQUERYTYPE_EVENT) ? 4 : 0);
}
// (this, dwIssueFlags): D3DISSUE_END 1, D3DISSUE_BEGIN 2.
void Query_Issue(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t flags = arg(c, 1);
    if (q && q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        if (flags & 2) {
            host_d9_query_begin(q->id);
            query_state()[q->id] = 1;
        } else if (flags & 1) {
            host_d9_query_end(q->id);
            query_state()[q->id] = 2;
        }
    }
    com_ret(c, q ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// (this, pData, dwSize, dwGetDataFlags)
void Query_GetData(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!q) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t value = 1;
    if (q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        auto it = query_state().find(q->id);
        if (it != query_state().end()) {
            uint32_t count = 0;
            int r = it->second == 2 ? host_d9_query_result(q->id, &count) : 0;
            if (r == 0) {
                com_ret(c, S_FALSE9); // not yet
                return;
            }
            value = r > 0 ? count : 1;
        }
    }
    if (data && size >= 4)
        wr32(data, value);
    com_ret(c, D3D_OK9);
}

#define RESOURCE_HEAD                                                                              \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetDevice", 2, Res_GetDevice},                              \
        {"SetPrivateData", 5, Res_SetPrivateData}, {"GetPrivateData", 4, Res_GetPrivateData},      \
        {"FreePrivateData", 2, Res_FreePrivateData}, {"SetPriority", 2, Res_SetPriority},          \
        {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad}, {                        \
        "GetType", 1, Res_GetType                                                                  \
    }

static const ComMethod g_vb9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, VB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_ib9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, IB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_surface9[] = {
    RESOURCE_HEAD,
    {"GetContainer", 3, Surf_GetContainer},
    {"GetDesc", 2, Surf_GetDesc},
    {"LockRect", 4, Surf_LockRect},
    {"UnlockRect", 1, Surf_UnlockRect},
    {"GetDC", 2, Surf_GetDC},
    {"ReleaseDC", 2, Surf_ReleaseDC},
};

static const ComMethod g_texture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetSurfaceLevel", 3, Tex_GetSurfaceLevel},
    {"LockRect", 5, Tex_LockRect},
    {"UnlockRect", 2, Tex_UnlockRect},
    {"AddDirtyRect", 2, Tex_AddDirtyRect},
};

static const ComMethod g_cubetexture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetCubeMapSurface", 4, Tex_GetCubeMapSurface},
    {"LockRect", 6, Tex_LockRectCube},
    {"UnlockRect", 3, Tex_UnlockRectCube},
    {"AddDirtyRect", 3, Tex_AddDirtyRectCube},
};

static const ComMethod g_ps9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetFunction", 3, PS_GetFunction},
};

static const ComMethod g_vs9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetFunction", 3, VS_GetFunction},
};

static const ComMethod g_decl9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetDeclaration", 3, Decl_GetDeclaration},
};

static const ComMethod g_query9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetType", 1, Query_GetType9},
    {"GetDataSize", 1, Query_GetDataSize},
    {"Issue", 2, Query_Issue},
    {"GetData", 4, Query_GetData},
};

// ---------------------------------------------------------------------------
// The pipeline record, and the device setters that write it
// ---------------------------------------------------------------------------
D9Pipeline &d9_pipeline(uint32_t device_id) {
    // Records are never erased and map nodes do not move, so the last one
    // asked for can be handed out again without a lookup.
    static auto *records = new std::map<uint32_t, D9Pipeline>();
    static thread_local uint32_t last_id = 0;
    static thread_local D9Pipeline *last = nullptr;
    if (last && last_id == device_id)
        return *last;
    last = &(*records)[device_id];
    last_id = device_id;
    return *last;
}

// (this, State, Value)
// (this, pViewport): D3DVIEWPORT9 is X, Y, Width, Height, MinZ, MaxZ.
void Dev_SetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        for (int i = 0; i < 4; ++i)
            p.viewport[i] = rd32(v + 4u * (uint32_t)i);
        uint32_t b0 = rd32(v + 16), b1 = rd32(v + 20);
        memcpy(&p.viewport_z[0], &b0, 4);
        memcpy(&p.viewport_z[1], &b1, 4);
        p.viewport_set = true;
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        ComObj *rt = com_get(dev->render_target);
        uint32_t whole[4] = {0, 0, rt ? rt->width : 640, rt ? rt->height : 480};
        for (int i = 0; i < 4; ++i)
            wr32(v + 4u * (uint32_t)i, p.viewport_set ? p.viewport[i] : whole[i]);
        float z0 = p.viewport_set ? p.viewport_z[0] : 0.0f,
              z1 = p.viewport_set ? p.viewport_z[1] : 1.0f;
        uint32_t b0, b1;
        memcpy(&b0, &z0, 4);
        memcpy(&b1, &z1, 4);
        wr32(v + 16, b0);
        wr32(v + 20, b1);
    }
    com_ret(c, D3D_OK9);
}

static uint32_t transform_slot(uint32_t state) {
    if (state == 2 || state == 3 || (state >= 16 && state <= 23))
        return state;
    if (state >= 256 && state <= 511)
        return state - 256 + 24;
    return 0xffffffffu;
}

// (this, State, pMatrix): world, view, projection and texture transforms.
void Dev_SetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), m = arg(c, 2);
    static uint32_t traced_set_transforms = 0;
    if (d9_vertex_trace() && traced_set_transforms++ < 32)
        LOGW("d3d9 vertex trace: SetTransform return=%08x state=%u matrix=%08x first=%08x %08x %08x %08x",
             rd32(c->r[R_ESP]), state, m, m ? rd32(m) : 0, m ? rd32(m + 4) : 0,
             m ? rd32(m + 8) : 0, m ? rd32(m + 12) : 0);
    uint32_t slot = transform_slot(state);
    if (!dev || !m || slot == 0xffffffffu) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 16; ++i) {
        uint32_t b = rd32(m + 4u * (uint32_t)i);
        memcpy(&pl.transform[slot][i], &b, 4);
    }
    pl.transform_set[slot] = true;
    com_ret(c, D3D_OK9);
}

void Dev_GetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), m = arg(c, 2);
    static uint32_t traced_get_transforms = 0;
    if (d9_vertex_trace() && traced_get_transforms++ < 32)
        LOGW("d3d9 vertex trace: GetTransform return=%08x state=%u matrix=%08x",
             rd32(c->r[R_ESP]), state, m);
    uint32_t slot = transform_slot(state);
    if (!dev || !m || slot == 0xffffffffu) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 16; ++i) {
        uint32_t b = 0;
        if (pl.transform_set[slot])
            memcpy(&b, &pl.transform[slot][i], 4);
        else if (i == 0 || i == 5 || i == 10 || i == 15)
            b = 0x3f800000u;
        wr32(m + 4u * (uint32_t)i, b);
    }
    if (d3d_color_trace()) {
        uint32_t ret = rd32(c->r[R_ESP]);
        if (ret == 0x0065e0a9u || ret == 0x0065e0bcu || ret == 0x0065e0cfu) {
            static uint32_t reported = 0;
            if (reported++ < 9) {
                LOGW("d3d9 color trace: GetTransform ret=%08x state=%u dst=%08x", ret, state, m);
                for (uint32_t row = 0; row < 4; ++row)
                    LOGW("d3d9 color trace: transform row%u=%08x,%08x,%08x,%08x", row,
                         rd32(m + row * 16), rd32(m + row * 16 + 4),
                         rd32(m + row * 16 + 8), rd32(m + row * 16 + 12));
            }
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, Type, Value)
void Dev_SetTextureStageState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2);
    if (dev && stage < 8 && type < 33)
        d9_pipeline(dev->id).tss[stage][type] = arg(c, 3);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1);
    if (d3d_rs_trace() && (state == 7 || state == 14 || state == 15 || state == 19 ||
                           state == 20 || state == 27 || state == 28)) {
        uint32_t old = dev && state < 256 ? d9_pipeline(dev->id).rs[state] : 0;
        uint32_t value = arg(c, 2);
        bool bad_blend = (state == 19 || state == 20) && (old == 0 || value == 0);
        bool bad_boolean = (state == 14 || state == 15 || state == 27 || state == 28) && value > 1;
        bool bad_depth = state == 7 && value > 2;
        if (bad_blend || bad_boolean || bad_depth)
            LOGW("d3d9 rs trace: SET ret=%08x device=%u state=%u old=%08x new=%08x version=%llu",
                 rd32(c->r[R_ESP]), dev ? dev->id : 0, state, old, value,
                 (unsigned long long)(dev ? d9_pipeline(dev->id).state_version : 0));
    }
    if (dev && state < 256) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.rs[state] != arg(c, 2) || !p.rs_set[state]) {
            p.rs[state] = arg(c, 2);
            p.rs_set[state] = true;
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Sampler, Type, Value)
void Dev_SetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2);
    if (dev && sampler < 16 && type < 14) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.sampler_state[sampler][type] != arg(c, 3)) {
            p.sampler_state[sampler][type] = arg(c, 3);
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// Copies Count four-float vectors from guest memory into a register table,
// clipped to the table.
static void write_registers(float (*table)[4], uint32_t limit, uint32_t start, uint32_t data,
                            uint32_t count) {
    for (uint32_t i = 0; i < count && start + i < limit && data; ++i)
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t bits = rd32(data + (i * 4 + k) * 4);
            memcpy(&table[start + i][k], &bits, 4);
        }
}

// (this, StartRegister, pConstantData, Vector4fCount)
void Dev_SetVertexShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        uint32_t start = arg(c, 1), data = arg(c, 2), count = arg(c, 3);
        if (d9_vertex_trace()) {
            static uint32_t traced = 0;
            if (traced++ < 128)
                LOGW("d3d9 vertex trace: SetVertexShaderConstantF return=%08x start=%u data=%08x count=%u first=%08x %08x %08x %08x",
                     rd32(c->r[R_ESP]), start, data, count, data ? rd32(data) : 0,
                     data ? rd32(data + 4) : 0, data ? rd32(data + 8) : 0,
                     data ? rd32(data + 12) : 0);
        }
        write_registers(d9_pipeline(dev->id).vconst, 256, start, data, count);
        if (d3d_color_trace() && data && start <= 10 && uint64_t(start) + count > 10) {
            uint32_t bits = rd32(data + (10 - start) * 16);
            if (bits >= 0x01000000u && bits < 0x02000000u) {
                static uint32_t reported = 0;
                if (reported++ < 12)
                    LOGW("d3d9 color trace: suspect c10 upload ret=%08x start=%u count=%u data=%08x c10bits=%08x draw=%u",
                         rd32(c->r[R_ESP]), start, count, data, bits, g_draws);
            }
        }
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetPixelShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        write_registers(d9_pipeline(dev->id).pconst, 32, arg(c, 1), arg(c, 2), arg(c, 3));
    com_ret(c, D3D_OK9);
}

static const ComMethod g_d3d9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, D9_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, D9_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D9_GetAdapterIdentifier},
    {"GetAdapterModeCount", 3, D9_GetAdapterModeCount},
    {"EnumAdapterModes", 5, D9_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, D9_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D9_CheckDeviceType},
    {"CheckDeviceFormat", 7, D9_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 7, D9_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D9_CheckDepthStencilMatch},
    {"CheckDeviceFormatConversion", 5, D9_CheckDeviceFormatConversion},
    {"GetDeviceCaps", 4, D9_GetDeviceCaps},
    {"GetAdapterMonitor", 2, D9_GetAdapterMonitor},
    {"CreateDevice", 7, D9_CreateDevice},
};

static const ComMethod g_device9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, Dev_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, Dev_GetAvailableTextureMem},
    {"EvictManagedResources", 1, Dev_EvictManagedResources},
    {"GetDirect3D", 2, Dev_GetDirect3D},
    {"GetDeviceCaps", 2, Dev_GetDeviceCaps},
    {"GetDisplayMode", 3, Dev_GetDisplayMode},
    {"GetCreationParameters", 2, Dev_GetCreationParameters},
    {"SetCursorProperties", 4, Dev_SetCursorProperties},
    {"SetCursorPosition", 4, Dev_SetCursorPosition},
    {"ShowCursor", 2, Dev_ShowCursor},
    {"CreateAdditionalSwapChain", 3, Dev_CreateAdditionalSwapChain},
    {"GetSwapChain", 3, Dev_GetSwapChain},
    {"GetNumberOfSwapChains", 1, Dev_GetNumberOfSwapChains},
    {"Reset", 2, Dev_Reset},
    {"Present", 5, Dev_Present},
    {"GetBackBuffer", 5, Dev_GetBackBuffer},
    {"GetRasterStatus", 3, Dev_GetRasterStatus},
    {"SetDialogBoxMode", 2, Dev_SetDialogBoxMode},
    {"SetGammaRamp", 4, Dev_SetGammaRamp},
    {"GetGammaRamp", 3, Dev_GetGammaRamp},
    {"CreateTexture", 9, Dev_CreateTexture},
    {"CreateVolumeTexture", 10, Dev_CreateVolumeTexture},
    {"CreateCubeTexture", 8, Dev_CreateCubeTexture},
    {"CreateVertexBuffer", 7, Dev_CreateVertexBuffer},
    {"CreateIndexBuffer", 7, Dev_CreateIndexBuffer},
    {"CreateRenderTarget", 9, Dev_CreateRenderTarget},
    {"CreateDepthStencilSurface", 9, Dev_CreateDepthStencilSurface},
    {"UpdateSurface", 5, Dev_UpdateSurface},
    {"UpdateTexture", 3, Dev_UpdateTexture},
    {"GetRenderTargetData", 3, Dev_GetRenderTargetData},
    {"GetFrontBufferData", 3, Dev_GetFrontBufferData},
    {"StretchRect", 6, Dev_StretchRect},
    {"ColorFill", 4, Dev_ColorFill},
    {"CreateOffscreenPlainSurface", 7, Dev_CreateOffscreenPlainSurface},
    {"SetRenderTarget", 3, Dev_SetRenderTarget},
    {"GetRenderTarget", 3, Dev_GetRenderTarget},
    {"SetDepthStencilSurface", 2, Dev_SetDepthStencilSurface},
    {"GetDepthStencilSurface", 2, Dev_GetDepthStencilSurface},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"SetRenderState", 3, Dev_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"CreateStateBlock", 3, Dev_CreateStateBlock},
    {"BeginStateBlock", 1, Dev_BeginStateBlock},
    {"EndStateBlock", 2, Dev_EndStateBlock},
    {"SetClipStatus", 2, Dev_SetClipStatus},
    {"GetClipStatus", 2, Dev_GetClipStatus},
    {"GetTexture", 3, Dev_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, Dev_GetTextureStageState},
    {"SetTextureStageState", 4, Dev_SetTextureStageState},
    {"GetSamplerState", 4, Dev_GetSamplerState},
    {"SetSamplerState", 4, Dev_SetSamplerState},
    {"ValidateDevice", 2, Dev_ValidateDevice},
    {"SetPaletteEntries", 3, Dev_SetPaletteEntries},
    {"GetPaletteEntries", 3, Dev_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, Dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, Dev_GetCurrentTexturePalette},
    {"SetScissorRect", 2, Dev_SetScissorRect},
    {"GetScissorRect", 2, Dev_GetScissorRect},
    {"SetSoftwareVertexProcessing", 2, Dev_SetSoftwareVertexProcessing},
    {"GetSoftwareVertexProcessing", 1, Dev_GetSoftwareVertexProcessing},
    {"SetNPatchMode", 2, Dev_SetNPatchMode},
    {"GetNPatchMode", 1, Dev_GetNPatchMode},
    {"DrawPrimitive", 4, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 7, Dev_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, Dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, Dev_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 7, Dev_ProcessVertices},
    {"CreateVertexDeclaration", 3, Dev_CreateVertexDeclaration},
    {"SetVertexDeclaration", 2, Dev_SetVertexDeclaration},
    {"GetVertexDeclaration", 2, Dev_GetVertexDeclaration},
    {"SetFVF", 2, Dev_SetFVF},
    {"GetFVF", 2, Dev_GetFVF},
    {"CreateVertexShader", 3, Dev_CreateVertexShader},
    {"SetVertexShader", 2, Dev_SetVertexShader},
    {"GetVertexShader", 2, Dev_GetVertexShader},
    {"SetVertexShaderConstantF", 4, Dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstantF", 4, Dev_GetVertexShaderConstantF},
    {"SetVertexShaderConstantI", 4, Dev_SetVertexShaderConstantI},
    {"GetVertexShaderConstantI", 4, Dev_GetVertexShaderConstantI},
    {"SetVertexShaderConstantB", 4, Dev_SetVertexShaderConstantB},
    {"GetVertexShaderConstantB", 4, Dev_GetVertexShaderConstantB},
    {"SetStreamSource", 5, Dev_SetStreamSource},
    {"GetStreamSource", 5, Dev_GetStreamSource},
    {"SetStreamSourceFreq", 3, Dev_SetStreamSourceFreq},
    {"GetStreamSourceFreq", 3, Dev_GetStreamSourceFreq},
    {"SetIndices", 2, Dev_SetIndices},
    {"GetIndices", 2, Dev_GetIndices},
    {"CreatePixelShader", 3, Dev_CreatePixelShader},
    {"SetPixelShader", 2, Dev_SetPixelShader},
    {"GetPixelShader", 2, Dev_GetPixelShader},
    {"SetPixelShaderConstantF", 4, Dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstantF", 4, Dev_GetPixelShaderConstantF},
    {"SetPixelShaderConstantI", 4, Dev_SetPixelShaderConstantI},
    {"GetPixelShaderConstantI", 4, Dev_GetPixelShaderConstantI},
    {"SetPixelShaderConstantB", 4, Dev_SetPixelShaderConstantB},
    {"GetPixelShaderConstantB", 4, Dev_GetPixelShaderConstantB},
    {"DrawRectPatch", 4, Dev_DrawRectPatch},
    {"DrawTriPatch", 4, Dev_DrawTriPatch},
    {"DeletePatch", 2, Dev_DeletePatch},
    {"CreateQuery", 3, Dev_CreateQuery},
};

// The DLL's one export. Not a COM method: the guest calls it directly.
void d3d9_Direct3DCreate9(X86 *c) {
    ComObj *o = com_new(K_D3D9);
    uint32_t view = o ? com_view(o, IF_D3D9) : 0;
    if (!view) {
        if (o)
            com_release(o);
        set_eax(c, 0);
        return;
    }
    LOGW("d3d9: Direct3DCreate9(SDK %u) -> %08x", arg(c, 0), view);
    set_eax(c, view);
}

static const ImportShim g_d3d9_exports[] = {
    {"d3d9.dll", "Direct3DCreate9", 1, d3d9_Direct3DCreate9},
};

static void device_destroy(ComObj *dev) {
    if (!dev)
        return;
    if (g_render_device_live.load() && g_render_device_id.load() == dev->id)
        g_render_device_live.store(false);
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.pixel_shader) {
        if (ComObj *sh = com_get(pl.pixel_shader))
            com_release(sh);
        pl.pixel_shader = 0;
    }
    if (pl.vertex_shader) {
        if (ComObj *sh = com_get(pl.vertex_shader))
            com_release(sh);
        pl.vertex_shader = 0;
    }
}

void d3d9_reset() {
    g_scratch = 0;
    g_scratch_size = 0;
    g_d3d9_focused.store(true);
    g_d3d9_focus_loss_epoch.store(0);
    g_render_device_live.store(false);
}

void d3d9_register() {
    static bool done = false;
    if (done)
        return;
    done = true;

    com_define(IF_D3D9, "d3d9.dll", "IDirect3D9", g_d3d9, std::size(g_d3d9));
    com_define(IF_D3DDEVICE9, "d3d9.dll", "IDirect3DDevice9", g_device9, std::size(g_device9));
    com_bind(IF_D3D9, K_D3D9);
    com_bind(IF_D3DDEVICE9, K_D3D9DEVICE);
    com_register_iid(IF_D3D9, IID_IDirect3D9_);
    com_register_iid(IF_D3DDEVICE9, IID_IDirect3DDevice9_);
    com_define(IF_D3DTEXTURE9, "d3d9.dll", "IDirect3DTexture9", g_texture9, std::size(g_texture9));
    com_define(IF_D3DCUBETEXTURE9, "d3d9.dll", "IDirect3DCubeTexture9", g_cubetexture9,
               std::size(g_cubetexture9));
    com_define(IF_D3DSURFACE9, "d3d9.dll", "IDirect3DSurface9", g_surface9, std::size(g_surface9));
    com_define(IF_D3DVERTEXBUFFER9, "d3d9.dll", "IDirect3DVertexBuffer9", g_vb9, std::size(g_vb9));
    com_define(IF_D3DINDEXBUFFER9, "d3d9.dll", "IDirect3DIndexBuffer9", g_ib9, std::size(g_ib9));
    com_define(IF_D3DVERTEXDECL9, "d3d9.dll", "IDirect3DVertexDeclaration9", g_decl9,
               std::size(g_decl9));
    com_define(IF_D3DQUERY9, "d3d9.dll", "IDirect3DQuery9", g_query9, std::size(g_query9));
    com_bind(IF_D3DTEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DCUBETEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DSURFACE9, K_D3D9SURFACE);
    com_bind(IF_D3DVERTEXBUFFER9, K_D3D9VB);
    com_bind(IF_D3DINDEXBUFFER9, K_D3D9IB);
    com_bind(IF_D3DVERTEXDECL9, K_D3D9DECL);
    com_bind(IF_D3DQUERY9, K_D3D9QUERY);
    com_define(IF_D3DPIXELSHADER9, "d3d9.dll", "IDirect3DPixelShader9", g_ps9, std::size(g_ps9));
    com_bind(IF_D3DPIXELSHADER9, K_D3D9PS);
    com_define(IF_D3DVERTEXSHADER9, "d3d9.dll", "IDirect3DVertexShader9", g_vs9, std::size(g_vs9));
    com_bind(IF_D3DVERTEXSHADER9, K_D3D9VS);
    com_set_destructor(K_D3D9DEVICE, device_destroy);
    for (ComKind k : {K_D3D9TEXTURE, K_D3D9SURFACE, K_D3D9VB, K_D3D9IB})
        com_set_destructor(k, d3d9_resource_destroy);
    com_set_destructor(K_D3D9QUERY, query_destroy);
    imports_register(g_d3d9_exports, std::size(g_d3d9_exports));
}
