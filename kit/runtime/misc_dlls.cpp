// Small DLL probes for a runtime with no printer, workstation service or tray.
#include "imports.h"
#include "memory.h"
#include "user32_internal.h"
#include "vorbis_decode.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#endif

namespace {
bool g_post_thx = false;

void dump_dwords(const char *tag, uint32_t addr, uint32_t n) {
    if (!addr || !gm_valid(addr, n * 4u)) {
        LOGW("%s %08x <unmapped>", tag, addr);
        return;
    }
    char buf[384];
    int pos = snprintf(buf, sizeof buf, "%s %08x:", tag, addr);
    for (uint32_t i = 0; i < n && pos > 0 && pos < (int)sizeof buf - 12; i++)
        pos += snprintf(buf + pos, sizeof buf - (size_t)pos, " %08x", rd32(addr + 4u * i));
    LOGW("%s", buf);
}

void dump_hud_vector(const char *label, uint32_t mgr, uint32_t off) {
    if (!mgr || !gm_valid(mgr + off + 8, 4)) {
        LOGW("hud: %s mgr=%08x <unmapped>", label, mgr);
        return;
    }
    uint32_t begin = rd32(mgr + off);
    uint32_t end = rd32(mgr + off + 4);
    uint32_t cap = rd32(mgr + off + 8);
    uint32_t count = (end >= begin && ((end - begin) & 3u) == 0) ? (end - begin) / 4u : 0;
    LOGW("hud: %s begin=%08x end=%08x cap=%08x count=%u", label, begin, end, cap, count);
    uint32_t n = count < 16u ? count : 16u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t slot = begin + 4u * i;
        if (!gm_valid(slot, 4))
            break;
        uint32_t obj = rd32(slot);
        uint32_t vt = (obj && gm_valid(obj, 8)) ? rd32(obj) : 0;
        uint32_t namep = (obj && gm_valid(obj + 4, 4)) ? rd32(obj + 4) : 0;
        LOGW("hud:   [%u] obj=%08x vt=%08x name=%08x \"%s\"", i, obj, vt, namep,
             namep ? gm_str(namep, 80).c_str() : "");
    }
}

void dump_class_rep(const char *label, uint32_t addr, uint32_t expect_vt) {
    if (!gm_valid(addr, 0x2cu)) {
        LOGW("hud: %s %08x <unmapped>", label, addr);
        return;
    }
    uint32_t vt = rd32(addr);
    uint32_t namep = rd32(addr + 0x14);
    LOGW("hud: %s %08x vt=%08x expect_vt=%08x namep=%08x \"%s\" linked=%s", label, addr, vt,
         expect_vt, namep, namep && gm_valid(namep, 1) ? gm_str(namep, 80).c_str() : "",
         vt == expect_vt ? "vptr-ok" : "vptr-mismatch");
}

constexpr uint32_t kHudTraceCap = 768;
struct HudTraceEvent {
    uint16_t seq;
    uint8_t phase; // 0=enter, 1=leave
    uint32_t eip;
    uint32_t ret;
    uint32_t ecx;
    uint32_t eax;
    uint32_t arg0;
    uint32_t list8;
    char name[56];
};
HudTraceEvent g_hud_ev[kHudTraceCap];
uint32_t g_hud_ev_n = 0;
uint32_t g_hud_ev_drop = 0;
uint16_t g_hud_seq = 0;
using HudIndexFn = int32_t (*)(uint32_t);
using HudBaseFn = void (*)(X86 *);
HudIndexFn g_hud_index_of = nullptr;
HudBaseFn const *g_hud_base_ptrs = nullptr;
uint8_t *g_hud_hooked = nullptr;
RecompHookFn *g_hud_hook_ptrs = nullptr;

const char *hud_trace_label(uint32_t eip) {
    switch (eip) {
    case 0x0047ffe0u:
        return "ScriptLoadCompiled";
    case 0x00489ee0u:
        return "ExecScript";
    case 0x00490390u:
        return "CompileExec";
    case 0x004922d0u:
        return "ConEvaluate";
    case 0x004956e0u:
        return "CreateByClassName";
    case 0x00438fd0u:
        return "FrontEnd.GUI_Initialize";
    case 0x004f1f90u:
        return "Presentation.Tick";
    case 0x004f2430u:
        return "Tutorial.s9";
    case 0x004f3440u:
        return "Tutorial.s1_Init";
    case 0x004f3490u:
        return "Tutorial.s2_Scripts";
    case 0x004f34c0u:
        return "Tutorial.s3_LoadData";
    case 0x004f3520u:
        return "Tutorial.s4_LoadComplete";
    case 0x004f3550u:
        return "Tutorial.s5_StartUp";
    case 0x004f3580u:
        return "Tutorial.s7";
    case 0x004f35f0u:
        return "Tutorial.sA";
    case 0x004f3680u:
        return "Presentation.Notify";
    case 0x005d1580u:
        return "HUDImageDisplay.create";
    case 0x005c4330u:
        return "HUDImageDisplay.ctor";
    case 0x005d1680u:
        return "LookTutorialHUD.create";
    case 0x005b7550u:
        return "LookTutorialHUD.ctor";
    case 0x005a52b0u:
        return "HUDManager2.lookup";
    case 0x005a5540u:
        return "HUDManager2.5a5540";
    case 0x005a55b0u:
        return "HUDManager2.5a55b0";
    case 0x005a5790u:
        return "HUDManager2.5a5790";
    case 0x005a62c0u:
        return "HUDManager2.5a62c0";
    case 0x005a6830u:
        return "HUDManager2.5a6830";
    case 0x005b6450u:
        return "LookTutorialHUD.tick";
    case 0x005c44e0u:
        return "LookTutorialInput";
    case 0x005b76b0u:
        return "LookTutorialHUD.advance";
    case 0x0042dd20u:
        return "Controller.alloc";
    case 0x0042dbe0u:
        return "Controller.create";
    case 0x006de750u:
        return "ControllerSystemDI.factory";
    case 0x006de3c0u:
        return "ControllerSystemDI.ctor";
    case 0x006df5d0u:
        return "Controller.platformInit";
    case 0x006df490u:
        return "Controller.platformGet";
    case 0x008e5181u:
        return "ole32.CoInitialize.thunk";
    case 0x008e10c6u:
        return "ole32.CoCreateInstance.thunk";
    case 0x008e36f0u:
        return "ole32.CoCreate.resolve";
    case 0x008e2e60u:
        return "ole32.CoCreate.clsid";
    case 0x008e0ebeu:
        return "ole32.CoCreate.iat";
    case 0x008e1b1bu:
        return "ole32.CoCreate.postcmp";
    case 0x006dcd40u:
        return "Controller.tickObservers";
    case 0x006de310u:
        return "Controller.enumDevices.cb";
    case 0x00438f90u:
        return "FrontEnd.DrawHandler";
    case 0x005a6a90u:
        return "HUDManager2.vt_5a6a90";
    default:
        return "guest";
    }
}

uint32_t hud_list8_count() {
    uint32_t mgr = gm_valid(0x827508u, 4) ? rd32(0x827508u) : 0;
    if (!mgr || !gm_valid(mgr + 12, 4))
        return 0xffffffffu;
    uint32_t begin = rd32(mgr + 8);
    uint32_t end = rd32(mgr + 12);
    if (end < begin || ((end - begin) & 3u))
        return 0xffffffffu;
    return (end - begin) / 4u;
}

void hud_ev_name(char *dst, size_t cap, uint32_t p) {
    dst[0] = 0;
    if (!p || !gm_valid(p, 1))
        return;
    std::string s = gm_str(p, cap - 1);
    std::snprintf(dst, cap, "%s", s.c_str());
}

void hud_trace_push(uint8_t phase, uint32_t eip, const X86 *c, const char *name) {
    if (g_hud_ev_n >= kHudTraceCap) {
        g_hud_ev_drop++;
        return;
    }
    HudTraceEvent &e = g_hud_ev[g_hud_ev_n++];
    e.seq = g_hud_seq++;
    e.phase = phase;
    e.eip = eip;
    e.ret = c && gm_valid(c->r[4], 4) ? rd32(c->r[4]) : 0;
    e.ecx = c ? c->r[1] : 0;
    e.eax = c ? c->r[0] : 0;
    e.arg0 = c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0;
    e.list8 = hud_list8_count();
    e.name[0] = 0;
    if (name && name[0])
        std::snprintf(e.name, sizeof e.name, "%s", name);
}

bool hud_interesting_create(const char *name) {
    if (!name || !name[0])
        return true;
    return std::strstr(name, "HUD") || std::strstr(name, "hud") || std::strstr(name, "Tutorial") ||
           std::strstr(name, "tutorial") || std::strstr(name, "stxef") ||
           std::strstr(name, "stxkoe") || std::strstr(name, "stxddn") ||
           std::strstr(name, "GameGroup") || std::strstr(name, "Look");
}

void hud_trace_hook_create(X86 *c, uint32_t index);
void hud_trace_hook_generic(X86 *c, uint32_t index);
void hud_trace_hook_look_tick(X86 *c, uint32_t index);
void hud_trace_hook_controller(X86 *c, uint32_t index);

#if defined(__APPLE__) || defined(__linux__)
using DinputCensusFn = void (*)(void);
using PulseDikFn = void (*)(uint8_t, bool);
using PulseMoveFn = void (*)(int32_t, int32_t);
DinputCensusFn host_dinput_log_census() {
    return reinterpret_cast<DinputCensusFn>(dlsym(RTLD_DEFAULT, "dinput_log_census"));
}
PulseDikFn host_pulse_dik() {
    return reinterpret_cast<PulseDikFn>(dlsym(RTLD_DEFAULT, "host_input_pulse_dik"));
}
PulseMoveFn host_pulse_move() {
    return reinterpret_cast<PulseMoveFn>(dlsym(RTLD_DEFAULT, "host_input_pulse_move"));
}
#endif

struct HudHookSpec {
    uint32_t eip;
    RecompHookFn fn;
};
HudHookSpec g_hud_specs[] = {
    {0x0047ffe0u, hud_trace_hook_generic}, {0x00489ee0u, hud_trace_hook_generic},
    {0x00490390u, hud_trace_hook_generic}, {0x004922d0u, hud_trace_hook_generic},
    {0x004956e0u, hud_trace_hook_create},  {0x00438fd0u, hud_trace_hook_generic},
    {0x004f1f90u, hud_trace_hook_generic}, {0x004f2430u, hud_trace_hook_generic},
    {0x004f3440u, hud_trace_hook_generic}, {0x004f3490u, hud_trace_hook_generic},
    {0x004f34c0u, hud_trace_hook_generic}, {0x004f3520u, hud_trace_hook_generic},
    {0x004f3550u, hud_trace_hook_generic}, {0x004f3580u, hud_trace_hook_generic},
    {0x004f35f0u, hud_trace_hook_generic}, {0x004f3680u, hud_trace_hook_generic},
    {0x005d1580u, hud_trace_hook_generic}, {0x005c4330u, hud_trace_hook_generic},
    {0x005d1680u, hud_trace_hook_generic}, {0x005b7550u, hud_trace_hook_generic},
    {0x005a52b0u, hud_trace_hook_generic}, {0x005a5540u, hud_trace_hook_generic},
    {0x005a55b0u, hud_trace_hook_generic}, {0x005a5790u, hud_trace_hook_generic},
    {0x005a62c0u, hud_trace_hook_generic}, {0x005a6830u, hud_trace_hook_generic},
    {0x005b6450u, hud_trace_hook_look_tick}, {0x005c44e0u, hud_trace_hook_generic},
    {0x005b76b0u, hud_trace_hook_generic},
    {0x0042dd20u, hud_trace_hook_controller}, {0x0042dbe0u, hud_trace_hook_controller},
    {0x006de750u, hud_trace_hook_controller}, {0x006de3c0u, hud_trace_hook_controller},
    {0x006df5d0u, hud_trace_hook_controller}, {0x006df490u, hud_trace_hook_controller},
    {0x008e5181u, hud_trace_hook_controller}, {0x008e10c6u, hud_trace_hook_controller},
    {0x008e36f0u, hud_trace_hook_controller}, {0x008e2e60u, hud_trace_hook_controller},
    {0x008e0ebeu, hud_trace_hook_controller}, {0x008e1b1bu, hud_trace_hook_controller},
    {0x006dcd40u, hud_trace_hook_controller}, {0x006de310u, hud_trace_hook_controller},
};
uint32_t g_hud_hot_seen[4] = {};
uint32_t g_tick_last_state = 0xffffffffu;
uint32_t g_tick_last_this = 0;

bool eval_string_interesting(const char *s) {
    if (!s || !s[0])
        return false;
    return std::strstr(s, "GUI_") || std::strstr(s, "HUD_") || std::strstr(s, "Tutorial") ||
           std::strstr(s, "tutorial") || std::strstr(s, "FrontEnd") || std::strstr(s, "Frontend") ||
           std::strstr(s, "frontend") || std::strstr(s, "SGS_") || std::strstr(s, "Opening") ||
           std::strstr(s, "LookTutorial") || std::strstr(s, "Title") || std::strstr(s, "MainMenu");
}

bool hud_rate_limited(uint32_t eip) {
    // Per-frame FE draw / manager tick proved live on the first run; do not
    // let them occupy the buffer. Keep a one-shot reach bit only.
    if (eip == 0x00438f90u) {
        if (g_hud_hot_seen[0]++)
            return true;
    } else if (eip == 0x005a6a90u) {
        if (g_hud_hot_seen[1]++)
            return true;
    } else if (eip == 0x00490390u) {
        static uint32_t n = 0;
        if (n++ >= 16)
            return true;
    } else if (eip == 0x005b6450u) {
        if (g_hud_hot_seen[2]++)
            return true;
    } else if (eip == 0x004f1f90u) {
        if (g_hud_hot_seen[3] > 8)
            return true;
        g_hud_hot_seen[3]++;
    } else if (eip == 0x00489ee0u) {
        static uint32_t n = 0;
        if (n++ >= 24)
            return true;
    } else if (eip == 0x004956e0u) {
        static uint32_t n = 0;
        if (n++ >= 24)
            return true;
    }
    return false;
}

void hud_fill_name(uint32_t eip, X86 *c, char *name, size_t cap) {
    name[0] = 0;
    if (eip == 0x0047ffe0u || eip == 0x004956e0u)
        hud_ev_name(name, cap, c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0);
    else if (eip == 0x004922d0u)
        hud_ev_name(name, cap, c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0);
    else if (eip == 0x00489ee0u || eip == 0x00490390u) {
        uint32_t argv = c && gm_valid(c->r[4] + 8, 4) ? rd32(c->r[4] + 8) : 0;
        uint32_t p = argv && gm_valid(argv, 4) ? rd32(argv) : 0;
        if (!p && argv && gm_valid(argv + 4, 4))
            p = rd32(argv + 4);
        hud_ev_name(name, cap, p);
    } else if (eip == 0x005a52b0u)
        hud_ev_name(name, cap, c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0);
    else if (eip == 0x004f1f90u || eip == 0x004f2430u || eip == 0x004f3440u || eip == 0x004f3490u ||
             eip == 0x004f34c0u || eip == 0x004f3520u || eip == 0x004f3550u || eip == 0x004f3580u ||
             eip == 0x004f35f0u) {
        uint32_t self = c ? c->r[1] : 0;
        uint32_t vt = self && gm_valid(self, 4) ? rd32(self) : 0;
        uint32_t st = self && gm_valid(self + 8, 4) ? rd32(self + 8) : 0xffffffffu;
        std::snprintf(name, cap, "this=%08x vt=%08x st=%u", self, vt, st);
    } else if (eip == 0x004f3680u) {
        uint32_t ev = c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0;
        std::snprintf(name, cap, "event=%u this=%08x", ev, c ? c->r[1] : 0);
    }
}

void look_tutorial_host_pulse(uint32_t ticks, int32_t state) {
#if defined(__APPLE__) || defined(__linux__)
    PulseDikFn pulse_dik = host_pulse_dik();
    PulseMoveFn pulse_move = host_pulse_move();
    if (!pulse_dik)
        return;
    // BEGIN (-2) only advances when LookTutorialInput runs. Drive the same
    // host path a player uses: DIK_RETURN, then SPACE, then relative mouse.
    if (state == -2 && ticks == 45) {
        LOGW("input: host pulse DIK_RETURN down (tutorial still BEGIN after %u ticks)", ticks);
        pulse_dik(0x1c, true);
    } else if (state == -2 && ticks == 60) {
        pulse_dik(0x1c, false);
        LOGW("input: host pulse DIK_SPACE down");
        pulse_dik(0x39, true);
    } else if (state == -2 && ticks == 75) {
        pulse_dik(0x39, false);
        if (pulse_move) {
            LOGW("input: host pulse mouse dx=80");
            pulse_move(80, 0);
        }
    } else if (state >= -1 && ticks % 90 == 0) {
        if (pulse_move)
            pulse_move((ticks & 1) ? 60 : -60, (ticks & 2) ? 40 : -40);
        pulse_dik(0x1c, ticks % 180 == 0);
        if (ticks % 180 == 30)
            pulse_dik(0x1c, false);
    }
#else
    (void)ticks;
    (void)state;
#endif
}

void hud_trace_hook_look_tick(X86 *c, uint32_t index) {
    uint32_t self = c ? c->r[1] : 0;
    int32_t state = (self && gm_valid(self + 0x68u, 4)) ? (int32_t)rd32(self + 0x68u) : 0x7fffffff;
    uint32_t layer = (self && gm_valid(self + 0x8cu, 4)) ? rd32(self + 0x8cu) : 0;
    uint32_t page = (self && gm_valid(self + 0x44u, 4)) ? rd32(self + 0x44u) : 0;
    uint32_t vis = (self && gm_valid(self + 0x60u, 4)) ? rd32(self + 0x60u) : 0;
    static uint32_t ticks = 0;
    static int32_t last_state = 0x7ffffffe;
    ticks++;
    if (ticks == 1 || state != last_state || ticks == 45 || ticks == 90) {
        LOGW("input: LookTutorialHUD.tick #%u this=%08x state=%d layer=%08x page=%08x "
             "flags+60=%08x skip=%u",
             ticks, self, state, layer, page, vis, gm_valid(0x827bfcu, 1) ? rd8(0x827bfcu) : 0xffu);
#if defined(__APPLE__) || defined(__linux__)
        if (DinputCensusFn census = host_dinput_log_census())
            census();
#endif
        last_state = state;
    }
    g_hud_base_ptrs[index](c);
    int32_t after = (self && gm_valid(self + 0x68u, 4)) ? (int32_t)rd32(self + 0x68u) : 0x7fffffff;
    if (after != state)
        LOGW("input: LookTutorialHUD.tick left state %d -> %d", state, after);
    look_tutorial_host_pulse(ticks, after);
}

void hud_trace_hook_controller(X86 *c, uint32_t index) {
    uint32_t eip = 0;
    for (size_t i = 0; i < sizeof g_hud_specs / sizeof g_hud_specs[0]; i++) {
        if (g_hud_index_of && g_hud_index_of(g_hud_specs[i].eip) == (int32_t)index) {
            eip = g_hud_specs[i].eip;
            break;
        }
    }
    uint32_t ret = c && gm_valid(c->r[4], 4) ? rd32(c->r[4]) : 0;
    uint32_t a0 = c && gm_valid(c->r[4] + 4, 4) ? rd32(c->r[4] + 4) : 0;
    uint32_t a1 = c && gm_valid(c->r[4] + 8, 4) ? rd32(c->r[4] + 8) : 0;
    uint32_t a2 = c && gm_valid(c->r[4] + 12, 4) ? rd32(c->r[4] + 12) : 0;
    uint32_t a3 = c && gm_valid(c->r[4] + 16, 4) ? rd32(c->r[4] + 16) : 0;
    uint32_t a4 = c && gm_valid(c->r[4] + 20, 4) ? rd32(c->r[4] + 20) : 0;
    uint32_t plat = gm_valid(0x85aef4u, 4) ? rd32(0x85aef4u) : 0xffffffffu;
    uint32_t pack = gm_valid(0x9c6dd6u, 1) ? rd8(0x9c6dd6u) : 0xffu;
    LOGW("input: %s enter eip=%08x ret=%08x ecx=%08x eax=%08x "
         "a0=%08x a1=%08x a2=%08x a3=%08x a4=%08x plat=%08x pack=%02x",
         hud_trace_label(eip), eip, ret, c ? c->r[1] : 0, c ? c->r[0] : 0, a0, a1, a2, a3, a4, plat,
         pack);
    if (eip == 0x008e10c6u && a0 && gm_valid(a0, 16))
        LOGW("input: CoCreate rclsid=%08x %08x %08x %08x", rd32(a0), rd32(a0 + 4), rd32(a0 + 8),
             rd32(a0 + 12));
    if (c && (eip == 0x008e0ebeu || eip == 0x006de3c0u || eip == 0x006dcd40u)) {
        uint32_t ebp = c->r[5];
        LOGW("input: frame ebp=%08x [+8]=%08x [+c]=%08x [+10]=%08x [+14]=%08x [+18]=%08x", ebp,
             ebp && gm_valid(ebp + 8, 4) ? rd32(ebp + 8) : 0,
             ebp && gm_valid(ebp + 12, 4) ? rd32(ebp + 12) : 0,
             ebp && gm_valid(ebp + 16, 4) ? rd32(ebp + 16) : 0,
             ebp && gm_valid(ebp + 20, 4) ? rd32(ebp + 20) : 0,
             ebp && gm_valid(ebp + 24, 4) ? rd32(ebp + 24) : 0);
    }
    if (eip == 0x006dcd40u) {
        uint32_t self = c ? c->r[1] : 0;
        uint32_t node = self && gm_valid(self + 0x28u, 4) ? rd32(self + 0x28u) : 0;
        uint32_t vt = node && gm_valid(node, 4) ? rd32(node) : 0;
        LOGW("input: tickObservers this=%08x list+28=%08x node0=%08x", self, node, vt);
    }
    hud_trace_push(0, eip, c, hud_trace_label(eip));
    g_hud_base_ptrs[index](c);
    uint32_t eax = c ? c->r[0] : 0;
    LOGW("input: %s leave eip=%08x eax=%08x ecx=%08x plat=%08x", hud_trace_label(eip), eip, eax,
         c ? c->r[1] : 0, gm_valid(0x85aef4u, 4) ? rd32(0x85aef4u) : 0xffffffffu);
#if defined(__APPLE__) || defined(__linux__)
    if (DinputCensusFn census = host_dinput_log_census())
        census();
#endif
    hud_trace_push(1, eip, c, hud_trace_label(eip));
}

void hud_trace_hook_generic(X86 *c, uint32_t index) {
    uint32_t eip = 0;
    for (size_t i = 0; i < sizeof g_hud_specs / sizeof g_hud_specs[0]; i++) {
        if (g_hud_index_of && g_hud_index_of(g_hud_specs[i].eip) == (int32_t)index) {
            eip = g_hud_specs[i].eip;
            break;
        }
    }
    char name[56] = {};
    hud_fill_name(eip, c, name, sizeof name);
    if (eip == 0x004922d0u && !eval_string_interesting(name)) {
        g_hud_base_ptrs[index](c);
        return;
    }
    if (eip == 0x004f1f90u) {
        uint32_t self = c ? c->r[1] : 0;
        uint32_t vt = self && gm_valid(self, 4) ? rd32(self) : 0;
        uint32_t st = self && gm_valid(self + 8, 4) ? rd32(self + 8) : 0xffffffffu;
        if (vt != 0x73db3cu && vt != 0x73d7e8u) {
            g_hud_base_ptrs[index](c);
            return;
        }
        if (self == g_tick_last_this && st == g_tick_last_state) {
            g_hud_base_ptrs[index](c);
            return;
        }
        g_tick_last_this = self;
        g_tick_last_state = st;
    }
    if (hud_rate_limited(eip)) {
        g_hud_base_ptrs[index](c);
        return;
    }
    hud_trace_push(0, eip, c, name);
    g_hud_base_ptrs[index](c);
    hud_fill_name(eip, c, name, sizeof name);
    hud_trace_push(1, eip, c, name);
}

void hud_trace_hook_create(X86 *c, uint32_t index) {
    char name[56] = {};
    hud_fill_name(0x004956e0u, c, name, sizeof name);
    if (!hud_interesting_create(name) && g_hud_ev_n > 200) {
        g_hud_base_ptrs[index](c);
        return;
    }
    hud_trace_push(0, 0x004956e0u, c, name);
    g_hud_base_ptrs[index](c);
    hud_trace_push(1, 0x004956e0u, c, name);
}

void hud_trace_install() {
#if defined(__APPLE__) || defined(__linux__)
    g_hud_index_of = reinterpret_cast<HudIndexFn>(dlsym(RTLD_DEFAULT, "recomp_index_of"));
    g_hud_base_ptrs = reinterpret_cast<HudBaseFn const *>(dlsym(RTLD_DEFAULT, "recomp_base_ptrs"));
    g_hud_hooked = reinterpret_cast<uint8_t *>(dlsym(RTLD_DEFAULT, "recomp_hooked"));
    g_hud_hook_ptrs = reinterpret_cast<RecompHookFn *>(dlsym(RTLD_DEFAULT, "recomp_hook_ptrs"));
    if (!g_hud_index_of || !g_hud_base_ptrs || !g_hud_hooked || !g_hud_hook_ptrs)
        return;
    int n = 0;
    for (auto &spec : g_hud_specs) {
        int32_t i = g_hud_index_of(spec.eip);
        if (i < 0)
            continue;
        g_hud_hook_ptrs[i] = spec.fn;
        __atomic_store_n(&g_hud_hooked[i], (uint8_t)1, __ATOMIC_RELEASE);
        n++;
    }
    if (n)
        LOGW("hud: lifecycle trace installed on %d guest functions", n);
#endif
}

struct HudTraceInit {
    HudTraceInit() {
        hud_trace_install();
    }
};
HudTraceInit g_hud_trace_init;
} // namespace

void recomp_post_intro_mark_thx() {
    g_post_thx = true;
}
bool recomp_post_intro_after_thx() {
    return g_post_thx;
}

void recomp_post_intro_dump(const char *tag) {
    recomp_post_intro_dump_cpu(tag, nullptr);
}

void recomp_hud_lifecycle_dump(const char *tag) {
    LOGW("hud: lifecycle tag=%s events=%u dropped=%u", tag ? tag : "", g_hud_ev_n, g_hud_ev_drop);
    for (uint32_t i = 0; i < g_hud_ev_n; i++) {
        const HudTraceEvent &e = g_hud_ev[i];
        LOGW("hud: ev %u %s %s eip=%08x ret=%08x ecx=%08x eax=%08x arg0=%08x list8=%u name=\"%s\"",
             e.seq, e.phase ? "leave" : "enter", hud_trace_label(e.eip), e.eip, e.ret, e.ecx, e.eax,
             e.arg0, e.list8, e.name);
    }
    if (tag && std::strstr(tag, "thx")) {
        g_hud_ev_n = 0;
        g_hud_ev_drop = 0;
        g_hud_seq = 0;
        g_tick_last_state = 0xffffffffu;
        g_tick_last_this = 0;
        LOGW("hud: lifecycle buffer reset after %s for post-intro census", tag);
    }
}

void recomp_post_intro_dump_cpu(const char *tag, const X86 *c) {
    LOGW("hud: dump tag=%s", tag ? tag : "");
    const char *t = tag ? tag : "";
    if (std::strstr(t, "null") || std::strstr(t, "thx") || std::strstr(t, "abort"))
        recomp_hud_lifecycle_dump(t);
    else
        LOGW("hud: lifecycle events=%u dropped=%u (full dump deferred)", g_hud_ev_n, g_hud_ev_drop);
    uint32_t slot0 = gm_valid(0x7d4108u, 4) ? rd32(0x7d4108u) : 0;
    LOGW("hud: EHE table[0] ptr=%08x \"%s\"", slot0,
         slot0 && gm_valid(slot0, 1) ? gm_str(slot0, 80).c_str() : "");
    dump_class_rep("HUDImageDisplay dynClassRep", 0x829110u, 0x755b80u);
    dump_class_rep("LookTutorialHUD dynClassRep", 0x82913cu, 0x755ba0u);
    uint32_t mgr = gm_valid(0x827508u, 4) ? rd32(0x827508u) : 0;
    LOGW("hud: HUDManager2 [0x827508]=%08x flags+0x58=%08x", mgr,
         mgr && gm_valid(mgr + 0x58u, 4) ? rd32(mgr + 0x58u) : 0);
    if (mgr && gm_valid(mgr, 0x5cu))
        dump_dwords("hud: manager", mgr, 24);
    dump_hud_vector("list+8", mgr, 8);
    dump_hud_vector("list+18", mgr, 0x18);
    LOGW("hud: FrontEnd [0x7bda08]=%08x [0x80c2a0]=%08x [0x80c288]=%02x",
         gm_valid(0x7bda08u, 4) ? rd32(0x7bda08u) : 0,
         gm_valid(0x80c2a0u, 4) ? rd32(0x80c2a0u) : 0,
         gm_valid(0x80c288u, 1) ? rd8(0x80c288u) : 0);
    uint32_t pres = gm_valid(0x822c18u, 4) ? rd32(0x822c18u) : 0;
    LOGW("hud: PresentationManager [0x822c18]=%08x flag822e75=%u flag822c14=%u", pres,
         gm_valid(0x822e75u, 1) ? rd8(0x822e75u) : 0xffu,
         gm_valid(0x822c14u, 1) ? rd8(0x822c14u) : 0xffu);
    if (pres && gm_valid(pres, 0x60u)) {
        dump_dwords("hud: pres", pres, 24);
        LOGW("hud: pres +14/+18 wait %08x/%08x", rd32(pres + 0x14u), rd32(pres + 0x18u));
        uint32_t hudst = rd32(pres + 0x4cu);
        uint32_t exo = rd32(pres + 0x50u);
        uint32_t mini = rd32(pres + 0x54u);
        uint32_t tut = rd32(pres + 0x58u);
        auto dump_ps = [](const char *lab, uint32_t obj, uint32_t expect) {
            uint32_t vt = obj && gm_valid(obj, 12) ? rd32(obj) : 0;
            uint32_t st = obj && gm_valid(obj + 8, 4) ? rd32(obj + 8) : 0;
            LOGW("hud: %s %08x vt=%08x st=%u expect_vt=%08x", lab, obj, vt, st, expect);
        };
        dump_ps("HUDPres +4c", hudst, 0x73d7e8u);
        dump_ps("Exotics +50", exo, 0x73d840u);
        dump_ps("MiniGame +54", mini, 0x73d8b0u);
        dump_ps("Tutorial +58", tut, 0x73db3cu);
    }
    dump_dwords("hud: class-list 0x8193c0", 0x8193c0u, 8);
    if (c) {
        uint32_t self = c->r[R_EDI];
        LOGW("hud: cpu EAX=%08x ESI=%08x EDI=%08x (LookTutorialHUD this?)", c->r[R_EAX],
             c->r[R_ESI], self);
        if (self && gm_valid(self, 0x90u)) {
            uint32_t namep = rd32(self + 4);
            LOGW("hud: this vt=%08x name=%08x \"%s\" state+0x68=%08x page+0x44=%08x", rd32(self),
                 namep, namep ? gm_str(namep, 80).c_str() : "", rd32(self + 0x68u),
                 rd32(self + 0x44u));
        }
    }
}

namespace {
void zero_out(uint32_t p) {
    if (p && gm_valid(p, 4))
        wr32(p, 0);
}
void yes(X86 *c) {
    set_eax(c, 1);
}
void zero(X86 *c) {
    set_eax(c, 0);
}
void session_service_unavailable(X86 *c) {
    // No Remote Desktop Services endpoint is running in the guest runtime.
    // Expose the exports so callers receive the service error as a BOOL result.
    set_last_error(1702); // RPC_S_INVALID_BINDING
    set_eax(c, 0);
}
void buffered_paint_unavailable(X86 *c) {
    set_eax(c, 0x80004001u); // E_NOTIMPL: use the caller's ordinary GDI path.
}
// Visual styles are on, as on every Windows since Vista, but there is no theme
// data: OpenThemeData finds none, so a program draws its controls the classic
// way, and the per-part queries fail with E_HANDLE as they do for a NULL theme.
// A VCL program binds every export at start and calls through the pointers,
// so each one is here. DrawThemeParentBackground is the one that does work.
void e_handle(X86 *c) {
    set_eax(c, 0x80070006u); // E_HANDLE
}
void theme_app_properties(X86 *c) {
    set_eax(c, 7); // STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT
}
// With no theme, the system metric queries answer from the system settings.
void theme_sys_value(X86 *c, const char *user32_export) {
    set_eax(c, guest_call(c, imports_resolve("USER32.dll", user32_export), arg(c, 1)));
}
void theme_sys_color(X86 *c) {
    theme_sys_value(c, "GetSysColor");
}
void theme_sys_color_brush(X86 *c) {
    theme_sys_value(c, "GetSysColorBrush");
}
void theme_sys_size(X86 *c) {
    theme_sys_value(c, "GetSystemMetrics");
}
// With no theme data, themed text is drawn the classic way: the DC's font, the
// colour DTTOPTS names if it names one, and no background, as themed text never
// has one. Windows answers a NULL theme with E_HANDLE, but a VCL program with
// styles on draws its captions through these calls, and would lose them all.
uint32_t theme_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t flags, uint32_t rect,
                    uint32_t options) {
    auto *dc = gdi::dc_of(hdc);
    if (!dc || !text || !rect)
        return 0x80070057u; // E_INVALIDARG
    const uint32_t option_flags = options && gm_valid(options, 12) ? rd32(options + 4) : 0;
    const uint32_t old_color = dc->text_color;
    const int old_mode = dc->bk_mode;
    if (option_flags & 0x1) // DTT_TEXTCOLOR
        dc->text_color = rd32(options + 8);
    if (option_flags & 0x200) // DTT_CALCRECT
        flags |= 0x400;       // DT_CALCRECT
    dc->bk_mode = 1;          // TRANSPARENT
    gdi::draw_text(hdc, text, count, rect, flags);
    if (auto *after = gdi::dc_of(hdc)) {
        after->text_color = old_color;
        after->bk_mode = old_mode;
    }
    return 0;
}
// DrawThemeText(hTheme, hdc, part, state, text, count, flags, flags2, rect)
void draw_theme_text(X86 *c) {
    set_eax(c, theme_text(arg(c, 1), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 8), 0));
}
// DrawThemeTextEx(hTheme, hdc, part, state, text, count, flags, rect, options)
void draw_theme_text_ex(X86 *c) {
    set_eax(c, theme_text(arg(c, 1), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8)));
}
// No paint buffer and no animation: the caller paints its target directly.
void begin_buffered_paint(X86 *c) {
    if (recomp_env("TRACE_GDI")) {
        const uint32_t rc = arg(c, 1);
        LOGW("gdi: BeginBufferedPaint target=%08x rect=%d,%d,%d,%d format=%u", arg(c, 0),
             rc && gm_valid(rc, 16) ? (int)rd32(rc) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 4) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 8) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 12) : -1, arg(c, 2));
    }
    zero_out(arg(c, 4)); // *phdc
    set_eax(c, 0);
}
void begin_buffered_animation(X86 *c) {
    zero_out(arg(c, 6)); // *phdcFrom
    zero_out(arg(c, 7)); // *phdcTo
    set_eax(c, 0);
}
// DrawThemeParentBackground(hwnd, hdc, prc): the parent paints its background
// into the child's DC, the DC's origin moved so the parent's coordinates land
// on the child, through WM_ERASEBKGND and then WM_PRINTCLIENT. A transparent
// control - the VCL's TStaticText - draws its text over what that leaves.
void draw_parent_background(X86 *c) {
    uint32_t hwnd = arg(c, 0), hdc = arg(c, 1), rect = arg(c, 2);
    auto *w = user32::find_window(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!w || !dc) {
        set_eax(c, 0x80070006u); // E_HANDLE
        return;
    }
    const uint32_t parent = w->parent;
    if (!parent) {
        set_eax(c, 0);
        return;
    }
    int32_t cx = 0, cy = 0, px = 0, py = 0;
    user32::client_origin(hwnd, &cx, &cy);
    user32::client_origin(parent, &px, &py);
    const gdi::DcState saved = *dc;
    if (rect && gm_valid(rect, 16)) { // prc limits the paint, in the child's coordinates
        gdi::Rect cut = gdi::to_device(hdc, {int32_t(rd32(rect)), int32_t(rd32(rect + 4)),
                                             int32_t(rd32(rect + 8)), int32_t(rd32(rect + 12))});
        std::vector<gdi::Rect> clip;
        for (gdi::Rect r : dc->clipped ? dc->clip : std::vector<gdi::Rect>{cut}) {
            gdi::Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                          std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                clip.push_back(hit);
        }
        dc->clip = std::move(clip);
        dc->clipped = true;
    }
    dc->org_x += cx - px;
    dc->org_y += cy - py;
    host_dispatch_to_wndproc(c, parent, 0x0014 /* WM_ERASEBKGND */, hdc, 0);
    host_dispatch_to_wndproc(c, parent, 0x0318 /* WM_PRINTCLIENT */, hdc, 4 /* PRF_CLIENT */);
    if (auto *after = gdi::dc_of(hdc)) // the window procedures may have changed it
        static_cast<gdi::DcState &>(*after) = saved;
    set_eax(c, 0);
}
void composition_enabled(X86 *c) {
    uint32_t out = arg(c, 0);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, 0x80070057u); // E_INVALIDARG
        return;
    }
    wr32(out, 0); // No desktop composition or glass frame in the guest model.
    set_eax(c, 0);
}
void enum_printers(X86 *c) {
    zero_out(arg(c, 5));
    zero_out(arg(c, 6));
    set_eax(c, 1);
}
void default_printer(X86 *c) {
    zero_out(arg(c, 1));
    set_last_error(2);
    set_eax(c, 0);
}
void open_printer(X86 *c) {
    zero_out(arg(c, 1));
    set_last_error(1801); // ERROR_INVALID_PRINTER_NAME
    set_eax(c, 0);
}
void document_properties(X86 *c) {
    set_eax(c, 0xffffffffu);
}
// The Win32 prototype has three arguments, not four: server, level, buffer.
void workstation_info(X86 *c) {
    zero_out(arg(c, 2));
    set_eax(c, 50);
}
void crt_memcpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), size = arg(c, 2);
    if (gm_valid(dst, size) && gm_valid(src, size) && size)
        memcpy(g_mem + dst, g_mem + src, size);
    set_eax(c, dst);
}
void crt_memmove(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), size = arg(c, 2);
    if (gm_valid(dst, size) && gm_valid(src, size) && size)
        memmove(g_mem + dst, g_mem + src, size);
    set_eax(c, dst);
}
// The C locale's toupper is used while normalizing archive names. Keep the
// result in the guest's int domain; a zero stub would erase each source byte.
void crt_toupper(X86 *c) {
    uint32_t ch = arg(c, 0);
    if (ch >= 'a' && ch <= 'z')
        ch -= 'a' - 'A';
    set_eax(c, ch);
}
void crt_strncpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), count = arg(c, 2);
    if (gm_valid(dst, count)) {
        uint32_t copied = 0;
        while (copied < count && gm_valid(src, copied + 1)) {
            if (!rd8(src + copied++))
                break;
        }
        if (copied == count || (copied && rd8(src + copied - 1) == 0)) {
            memcpy(g_mem + dst, g_mem + src, copied);
            if (copied < count)
                memset(g_mem + dst + copied, 0, count - copied);
        }
    }
    set_eax(c, dst);
}
void crt_strrchr(X86 *c) {
    uint32_t source = arg(c, 0);
    uint8_t needle = (uint8_t)arg(c, 1);
    uint32_t last = 0;
    if (source && gm_valid(source, 1)) {
        for (uint32_t at = source; gm_valid(at, 1); ++at) {
            uint8_t value = rd8(at);
            if (value == needle)
                last = at;
            if (!value)
                break; // searching for NUL includes the terminator
        }
    }
    set_eax(c, last);
}
void crt_strchr(X86 *c) {
    uint32_t source = arg(c, 0);
    uint8_t needle = (uint8_t)arg(c, 1);
    if (source && gm_valid(source, 1)) {
        for (uint32_t at = source; gm_valid(at, 1); ++at) {
            uint8_t value = rd8(at);
            if (value == needle) {
                set_eax(c, at);
                return;
            }
            if (!value)
                break;
        }
    }
    set_eax(c, 0);
}
void crt_strstr(X86 *c) {
    uint32_t haystack = arg(c, 0), needle = arg(c, 1);
    if (!haystack || !needle || !gm_valid(haystack, 1) || !gm_valid(needle, 1)) {
        set_eax(c, 0);
        return;
    }
    if (rd8(needle) == 0) {
        set_eax(c, haystack);
        return;
    }
    for (uint32_t at = haystack; gm_valid(at, 1) && rd8(at); ++at) {
        uint32_t h = at, n = needle;
        while (gm_valid(h, 1) && gm_valid(n, 1) && rd8(n) && rd8(h) == rd8(n)) {
            ++h;
            ++n;
        }
        if (gm_valid(n, 1) && rd8(n) == 0) {
            set_eax(c, at);
            return;
        }
    }
    set_eax(c, 0);
}
void crt_strncmp(X86 *c) {
    uint32_t left = arg(c, 0), right = arg(c, 1), count = arg(c, 2);
    int difference = 0;
    for (uint32_t i = 0; i < count && gm_valid(left, i + 1) && gm_valid(right, i + 1); ++i) {
        uint8_t a = rd8(left + i), b = rd8(right + i);
        difference = (int)a - (int)b;
        if (difference || !a)
            break;
    }
    set_eax(c, (uint32_t)difference);
}
static uint8_t crt_ascii_lower(uint8_t value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}
void crt_strcmpi(X86 *c) {
    uint32_t left = arg(c, 0), right = arg(c, 1);
    int difference = 0;
    for (uint32_t i = 0; gm_valid(left, i + 1) && gm_valid(right, i + 1); ++i) {
        uint8_t a = crt_ascii_lower(rd8(left + i));
        uint8_t b = crt_ascii_lower(rd8(right + i));
        difference = (int)a - (int)b;
        if (difference || !a)
            break;
    }
    set_eax(c, (uint32_t)difference);
}
static thread_local uint32_t crt_token_next = 0;
static bool crt_token_delimiter(uint32_t delimiters, uint8_t value) {
    for (uint32_t at = delimiters; gm_valid(at, 1) && rd8(at); ++at)
        if (rd8(at) == value)
            return true;
    return false;
}
void crt_strtok(X86 *c) {
    uint32_t source = arg(c, 0), delimiters = arg(c, 1);
    if (source)
        crt_token_next = source;
    if (!crt_token_next || !delimiters || !gm_valid(crt_token_next, 1) ||
        !gm_valid(delimiters, 1)) {
        set_eax(c, 0);
        return;
    }
    uint32_t at = crt_token_next;
    while (gm_valid(at, 1) && rd8(at) && crt_token_delimiter(delimiters, rd8(at)))
        ++at;
    if (!gm_valid(at, 1) || !rd8(at)) {
        crt_token_next = 0;
        set_eax(c, 0);
        return;
    }
    uint32_t token = at;
    while (gm_valid(at, 1) && rd8(at) && !crt_token_delimiter(delimiters, rd8(at)))
        ++at;
    if (gm_valid(at, 1) && rd8(at)) {
        wr8(at, 0);
        crt_token_next = at + 1;
    } else {
        crt_token_next = 0;
    }
    set_eax(c, token);
}
// void __cdecl _initterm(pfbegin, pfend). MSVC 7.1 walks [pfbegin, pfend) and
// calls each non-null void(*)(void), skipping the null slots the linker leaves
// between contributors. Scarface's C++ initializers are not an in-image walker:
// 0x00702A2C pushes the table and calls msvcr71!_initterm. Returning 0 here
// leaves those objects at their BSS zeros, so the first virtual call through
// one faults. cdecl: the caller pops both pointers.
// msvcr71!_CIfmod is not a stack-argument fmod. The pinned 7.10.3052.4 body
// swaps ST(0) with ST(1), repeats FPREM until the reduction is complete, then
// FSTP ST(1). The remainder of the old ST(1) by the old ST(0) stays in ST(0).
// Deeper registers move up one slot. EAX is not the result.
void fpu_binary_result(X86 *c, double result, uint16_t status) {
    fdrop(c);
    fdrop(c);
    fpush(c, result);
    c->fpu_sw = status;
}

double crt_fprem_complete(X86 *c, double x, double y) {
    double acc = x;
    for (int step = 0; step < 64; ++step) {
        acc = fprem_common(c, acc, y, 0);
        if ((c->fpu_sw & 0x0400u) == 0)
            return acc;
    }
    return acc;
}

// msvcr71!_CIpow, with [0x7C38B300] still 0, swaps the two registers and
// stores them as pow(old ST(1), old ST(0)). The internal routine then
// leaves that one value on the x87 stack. A non-integer power of a
// negative base is the invalid operation.
void crt_CIpow(X86 *c) {
    if (fempty(c, 0) || fempty(c, 1)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGW("_CIpow called without two x87 operands");
        }
        return;
    }
    double exponent = ST(c, 0);
    double base = ST(c, 1);
    uint16_t status = (uint16_t)(c->fpu_sw & (uint16_t)~0x4700u);
    double result;
    bool exponent_is_int = exponent == trunc(exponent) && fabs(exponent) <= 9007199254740992.0;
    if (isnan(base) || isnan(exponent) || (base < 0.0 && !exponent_is_int)) {
        status = (uint16_t)(status | 0x0001u);
        result = x87_indefinite();
    } else {
        result = pow(base, exponent);
        if (isnan(result)) {
            status = (uint16_t)(status | 0x0001u);
            result = x87_indefinite();
        }
    }
    fpu_binary_result(c, result, status);
}

void crt_CIfmod(X86 *c) {
    if (fempty(c, 0) || fempty(c, 1)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGW("_CIfmod called without two x87 operands");
        }
        return;
    }
    double y = ST(c, 0);
    double x = ST(c, 1);
    double result = crt_fprem_complete(c, x, y);
    fpu_binary_result(c, result, c->fpu_sw);
}

// MSVCR71 starts in the C locale. Scarface does not import setlocale, and the
// run that reached these calls never invoked a locale API. The reached
// consumers are `test eax, eax`, so the contract that matters is zero versus
// nonzero. EOF is -1 and is in none of these classes. A value outside
// {-1, 0..255} is not a CRT character; it is logged and rejected.
enum { CTYPE_SPACE = 1, CTYPE_DIGIT = 2, CTYPE_ALPHA = 4 };

int crt_ctype_class(int32_t value) {
    if (value == -1)
        return 0;
    if (value < 0 || value > 255) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGW("ctype argument %d is outside EOF and 0..255", (int)value);
        }
        return 0;
    }
    uint8_t ch = (uint8_t)value;
    int bits = 0;
    if (ch == ' ' || (ch >= 0x09 && ch <= 0x0d))
        bits |= CTYPE_SPACE;
    if (ch >= '0' && ch <= '9')
        bits |= CTYPE_DIGIT;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
        bits |= CTYPE_ALPHA;
    return bits;
}

void crt_ctype_pred(X86 *c, int bit) {
    int32_t value = (int32_t)arg(c, 0);
    set_eax(c, (crt_ctype_class(value) & bit) ? 1u : 0u);
}
void crt_isspace(X86 *c) { crt_ctype_pred(c, CTYPE_SPACE); }
void crt_isdigit(X86 *c) { crt_ctype_pred(c, CTYPE_DIGIT); }
void crt_isalpha(X86 *c) { crt_ctype_pred(c, CTYPE_ALPHA); }
void crt_isalnum(X86 *c) { crt_ctype_pred(c, CTYPE_ALPHA | CTYPE_DIGIT); }

// Guest x86 va_list is an address. It is not a host ARM64 va_list.
struct GuestVaCursor32 {
    uint32_t address;
    uint32_t u32() {
        uint32_t value = rd32(address);
        address += 4;
        return value;
    }
    int32_t s32() { return (int32_t)u32(); }
};

// The reached call is vsprintf(dst, "CalloutText%02i", &index). Its return
// count is discarded; the caller uses the bytes written at dst. Conversions
// outside this subset stay on the old write-nothing path so an earlier call
// does not change behavior until that conversion is captured.
void crt_format_decimal(std::string *out, int32_t value, bool zero_pad, unsigned width) {
    uint32_t magnitude;
    bool negative = value < 0;
    if (value == (int32_t)0x80000000u)
        magnitude = 0x80000000u;
    else
        magnitude = negative ? (uint32_t)(-value) : (uint32_t)value;
    char digits[16];
    int count = 0;
    do {
        digits[count++] = (char)('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude);
    unsigned body = (unsigned)count + (negative ? 1u : 0u);
    unsigned pad = width > body ? width - body : 0u;
    if (!zero_pad)
        out->append(pad, ' ');
    if (negative)
        out->push_back('-');
    if (zero_pad)
        out->append(pad, '0');
    while (count)
        out->push_back(digits[--count]);
}

bool crt_vsprintf_subset(std::string *out, uint32_t format, GuestVaCursor32 *args) {
    for (uint32_t at = format; gm_valid(at, 1); ++at) {
        uint8_t ch = rd8(at);
        if (!ch)
            return true;
        if (ch != '%') {
            out->push_back((char)ch);
            continue;
        }
        if (!gm_valid(at + 1, 1))
            return false;
        uint8_t next = rd8(++at);
        if (next == '%') {
            out->push_back('%');
            continue;
        }
        bool zero_pad = false;
        while (next == '0') {
            zero_pad = true;
            if (!gm_valid(at + 1, 1))
                return false;
            next = rd8(++at);
        }
        unsigned width = 0;
        if (next == '*' || next == 'h' || next == 'I')
            return false;
        while (next >= '0' && next <= '9') {
            width = width * 10u + (unsigned)(next - '0');
            if (width > 64)
                return false;
            if (!gm_valid(at + 1, 1))
                return false;
            next = rd8(++at);
        }
        int precision = -1;
        if (next == '.') {
            precision = 0;
            if (!gm_valid(at + 1, 1))
                return false;
            next = rd8(++at);
            while (next >= '0' && next <= '9') {
                precision = precision * 10 + (next - '0');
                if (precision > 64 || !gm_valid(at + 1, 1))
                    return false;
                next = rd8(++at);
            }
        }
        // long is 32 bits on this target, so %l only names the same argument.
        if (next == 'l') {
            if (!gm_valid(at + 1, 1))
                return false;
            next = rd8(++at);
        }
        if (next == 'u' || next == 'x' || next == 'X' ||
            ((next == 'd' || next == 'i') && precision >= 0)) {
            uint32_t value = args->u32();
            bool negative = (next == 'd' || next == 'i') && (int32_t)value < 0;
            uint32_t magnitude = negative ? 0u - value : value;
            unsigned radix = next == 'x' || next == 'X' ? 16u : 10u;
            const char *digits = next == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            std::string body;
            if (magnitude || precision != 0) {
                do {
                    body.insert(body.begin(), digits[magnitude % radix]);
                    magnitude /= radix;
                } while (magnitude);
            }
            if (precision > 0 && body.size() < (size_t)precision)
                body.insert(0, (size_t)precision - body.size(), '0');
            size_t total = body.size() + (negative ? 1 : 0);
            // A precision turns off the 0 flag for integers.
            bool pad_zero = zero_pad && precision < 0;
            if (!pad_zero && width > total)
                out->append(width - total, ' ');
            if (negative)
                out->push_back('-');
            if (pad_zero && width > total)
                out->append(width - total, '0');
            out->append(body);
            continue;
        }
        if (next == 'c') {
            if (zero_pad || precision >= 0)
                return false;
            if (width > 1)
                out->append(width - 1, ' ');
            out->push_back((char)(args->u32() & 0xffu));
            continue;
        }
        if (next == 's') {
            if (width || zero_pad || precision >= 0)
                return false;
            uint32_t text = args->u32();
            if (!text)
                return false;
            for (uint32_t i = 0; i < 4096u; ++i) {
                if (!gm_valid(text + i, 1))
                    return false;
                uint8_t byte = rd8(text + i);
                if (!byte)
                    break;
                out->push_back((char)byte);
                if (i == 4095u)
                    return false;
            }
            continue;
        }
        if (next != 'd' && next != 'i')
            return false;
        crt_format_decimal(out, args->s32(), zero_pad, width);
    }
    return false;
}

void crt_write_formatted(X86 *c, uint32_t dst, uint32_t format, GuestVaCursor32 *args) {
    std::string out;
    if (!format || !crt_vsprintf_subset(&out, format, args)) {
        static unsigned logged = 0;
        if (logged++ < 4) {
            std::string fmt = format && gm_valid(format, 1) ? gm_str(format) : std::string();
            if (fmt.size() > 80)
                fmt.resize(80);
            LOGW("guest printf subset does not cover this call (dst=%08x format=%08x \"%s\"); "
                 "leaving the buffer unchanged",
                 dst, format, fmt.c_str());
        }
        set_eax(c, 0);
        return;
    }
    if (dst && gm_valid(dst, (uint32_t)out.size() + 1))
        memcpy(g_mem + dst, out.c_str(), out.size() + 1);
    set_eax(c, (uint32_t)out.size());
}

// int *__cdecl __p__fmode(void) and __p__commode(void). The CRT startup
// writes the guest's chosen mode through the returned pointer. A null return
// stores that mode at guest address 0.
uint32_t crt_process_int(uint32_t *slot) {
    if (!*slot)
        *slot = heap_alloc(4, true);
    return *slot;
}
uint32_t g_crt_fmode = 0;
uint32_t g_crt_commode = 0;
// msvcr71 rand/srand keep the seed at per-thread data + 0x14.
// seed = seed * 0x343fd + 0x269ec3; return (seed >> 16) & 0x7fff.
// The reached call seeds first, then pushes rand() into a mixer that
// replaces a zero result with a constant.
uint32_t g_crt_rand_seed = 1;
void crt_srand(X86 *c) { g_crt_rand_seed = arg(c, 0); }
void crt_rand(X86 *c) {
    g_crt_rand_seed = g_crt_rand_seed * 0x343fdu + 0x269ec3u;
    set_eax(c, (g_crt_rand_seed >> 16) & 0x7fffu);
}

void crt_p_fmode(X86 *c) { set_eax(c, crt_process_int(&g_crt_fmode)); }
void crt_p_commode(X86 *c) { set_eax(c, crt_process_int(&g_crt_commode)); }

void crt_vsprintf(X86 *c) {
    GuestVaCursor32 args{arg(c, 2)};
    crt_write_formatted(c, arg(c, 0), arg(c, 1), &args);
}

// int __cdecl _vsnprintf(char *dst, size_t count, const char *fmt, va_list).
// MSVCR71 writes at most count bytes. A result shorter than count is
// terminated and its length returned; exactly count bytes are written
// without a terminator and count is returned; a longer result writes count
// bytes, no terminator, and returns -1.
void crt_vsnprintf(X86 *c) {
    uint32_t dst = arg(c, 0), count = arg(c, 1), format = arg(c, 2);
    GuestVaCursor32 args{arg(c, 3)};
    std::string out;
    if (!format || !crt_vsprintf_subset(&out, format, &args)) {
        static unsigned logged = 0;
        if (logged++ < 4)
            LOGW("_vsnprintf: format %08x \"%s\" is outside the supported subset", format,
                 format && gm_valid(format, 1) ? gm_str(format, 80).c_str() : "");
        set_eax(c, 0xffffffffu);
        return;
    }
    size_t n = std::min<size_t>(out.size(), count);
    if (n && (!dst || !gm_valid(dst, (uint32_t)n))) {
        set_eax(c, 0xffffffffu);
        return;
    }
    if (n)
        memcpy(g_mem + dst, out.data(), n);
    if (out.size() < count) {
        wr8(dst + (uint32_t)out.size(), 0);
        set_eax(c, (uint32_t)out.size());
    } else {
        set_eax(c, out.size() == count ? count : 0xffffffffu);
    }
}

bool crt_space(uint8_t ch) {
    return ch == ' ' || (ch >= 0x09 && ch <= 0x0d);
}

// char *_strlwr(char *) / _strupr(char *) convert ASCII letters in place, as
// in the C locale, and return the argument.
void crt_case_in_place(X86 *c, bool upper) {
    uint32_t s = arg(c, 0);
    for (uint32_t at = s; s && gm_valid(at, 1); ++at) {
        uint8_t ch = rd8(at);
        if (!ch)
            break;
        if (upper && ch >= 'a' && ch <= 'z')
            wr8(at, (uint8_t)(ch - 32));
        else if (!upper && ch >= 'A' && ch <= 'Z')
            wr8(at, (uint8_t)(ch + 32));
    }
    set_eax(c, s);
}
void crt_strlwr(X86 *c) { crt_case_in_place(c, false); }
void crt_strupr(X86 *c) { crt_case_in_place(c, true); }

// size_t strcspn(const char *s, const char *reject).
void crt_strcspn(X86 *c) {
    uint32_t s = arg(c, 0), reject = arg(c, 1), n = 0;
    while (s && gm_valid(s + n, 1) && rd8(s + n) && !crt_token_delimiter(reject, rd8(s + n)))
        ++n;
    set_eax(c, n);
}

// MSVCR71 atoi accumulates in a 32-bit int and wraps on overflow; it does
// not clamp the way strtol does.
void crt_atoi(X86 *c) {
    uint32_t at = arg(c, 0);
    if (!at || !gm_valid(at, 1)) {
        set_eax(c, 0);
        return;
    }
    while (gm_valid(at, 1) && crt_space(rd8(at)))
        ++at;
    bool negative = false;
    if (gm_valid(at, 1) && (rd8(at) == '-' || rd8(at) == '+'))
        negative = rd8(at++) == '-';
    uint32_t total = 0;
    while (gm_valid(at, 1) && rd8(at) >= '0' && rd8(at) <= '9')
        total = total * 10u + (uint32_t)(rd8(at++) - '0');
    set_eax(c, negative ? 0u - total : total);
}

// long strtol(const char *s, char **end, int base). Out-of-range values clamp
// to LONG_MIN/LONG_MAX. errno is not modelled. With no digits, *end = s.
void crt_strtol(X86 *c) {
    uint32_t s = arg(c, 0), end = arg(c, 1);
    int base = (int)arg(c, 2);
    uint32_t at = s;
    auto peek = [&](uint32_t a) -> uint8_t { return a && gm_valid(a, 1) ? rd8(a) : 0; };
    while (crt_space(peek(at)))
        ++at;
    bool negative = false;
    if (peek(at) == '-' || peek(at) == '+')
        negative = peek(at++) == '-';
    auto digit = [](uint8_t ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
        return 99;
    };
    if ((base == 0 || base == 16) && peek(at) == '0' && (peek(at + 1) | 0x20) == 'x' &&
        digit(peek(at + 2)) < 16) {
        at += 2;
        base = 16;
    } else if (base == 0) {
        base = peek(at) == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36) {
        if (end && gm_valid(end, 4))
            wr32(end, s);
        set_eax(c, 0);
        return;
    }
    uint64_t total = 0;
    bool any = false, overflow = false;
    for (int d; (d = digit(peek(at))) < base; ++at) {
        any = true;
        total = total * (uint64_t)base + (uint64_t)d;
        if (total > 0x80000000ull)
            overflow = true, total = 0x80000000ull;
    }
    uint32_t result;
    if (!any)
        result = 0, at = s;
    else if (negative)
        result = overflow || total > 0x80000000ull ? 0x80000000u : 0u - (uint32_t)total;
    else
        result = overflow || total > 0x7fffffffull ? 0x7fffffffu : (uint32_t)total;
    if (end && gm_valid(end, 4))
        wr32(end, at);
    set_eax(c, result);
}

// void *bsearch(key, base, num, width, compare), the MSVCR71 bsearch.c walk.
// The comparator is a guest cdecl function called through guest_call.
void crt_bsearch(X86 *c) {
    uint32_t key = arg(c, 0), base = arg(c, 1), num = arg(c, 2), width = arg(c, 3);
    uint32_t compare = arg(c, 4);
    uint32_t found = 0;
    if (num && width && compare && gm_valid(compare, 1) &&
        (uint64_t)num * width <= 0x10000000ull && gm_valid(base, num * width)) {
        uint32_t lo = base, hi = base + (num - 1) * width;
        while (lo <= hi) {
            uint32_t half = num / 2;
            if (!half) {
                if (num && !(int32_t)guest_call(c, compare, key, lo))
                    found = lo;
                break;
            }
            uint32_t mid = lo + ((num & 1) ? half : half - 1) * width;
            int32_t result = (int32_t)guest_call(c, compare, key, mid);
            if (!result) {
                found = mid;
                break;
            }
            if (result < 0) {
                hi = mid - width;
                num = (num & 1) ? half : half - 1;
            } else {
                lo = mid + width;
                num = half;
            }
        }
    }
    set_eax(c, found);
}

// double ceil(double): the argument is eight bytes on the stack and the
// result is returned in ST(0).
void crt_ceil(X86 *c) {
    uint64_t bits = (uint64_t)arg(c, 0) | ((uint64_t)arg(c, 1) << 32);
    double value;
    memcpy(&value, &bits, sizeof value);
    fpush(c, std::ceil(value));
}

void crt_floor(X86 *c) {
    uint64_t bits = (uint64_t)arg(c, 0) | ((uint64_t)arg(c, 1) << 32);
    double value;
    memcpy(&value, &bits, sizeof value);
    fpush(c, std::floor(value));
}

// msvcr71 7.10.3052.4 _fltin grammar after leading whitespace: [sign] digits
// [. digits] [e|E|d|D [sign] digits]. No hex, inf or nan. Returns the value
// and the guest address just past the accepted text (start if nothing parsed).
static double crt_decimal_prefix(uint32_t start, uint32_t *end) {
    auto peek = [](uint32_t a) -> uint8_t { return a && gm_valid(a, 1) ? rd8(a) : 0; };
    uint32_t at = start;
    while (crt_space(peek(at)))
        ++at;
    std::string text;
    if (peek(at) == '-' || peek(at) == '+')
        text.push_back((char)peek(at++));
    bool digits = false;
    while (peek(at) >= '0' && peek(at) <= '9')
        text.push_back((char)peek(at++)), digits = true;
    if (peek(at) == '.') {
        text.push_back((char)peek(at++));
        while (peek(at) >= '0' && peek(at) <= '9')
            text.push_back((char)peek(at++)), digits = true;
    }
    if (!digits) {
        *end = start;
        return 0.0;
    }
    uint8_t e = peek(at) | 0x20;
    if (e == 'e' || e == 'd') {
        uint32_t mark = at;
        std::string exp = "e";
        ++at;
        if (peek(at) == '-' || peek(at) == '+')
            exp.push_back((char)peek(at++));
        bool exp_digits = false;
        while (peek(at) >= '0' && peek(at) <= '9')
            exp.push_back((char)peek(at++)), exp_digits = true;
        if (exp_digits)
            text += exp;
        else
            at = mark;
    }
    *end = at;
    return strtod(text.c_str(), nullptr);
}

// double atof(const char *): result in ST(0).
void crt_atof(X86 *c) {
    uint32_t end;
    fpush(c, crt_decimal_prefix(arg(c, 0), &end));
}

// double strtod(const char *, char **end): result in ST(0).
void crt_strtod(X86 *c) {
    uint32_t s = arg(c, 0), endp = arg(c, 1), end;
    double value = crt_decimal_prefix(s, &end);
    if (endp && gm_valid(endp, 4))
        wr32(endp, end);
    fpush(c, value);
}

// msvcr71 _CIacos/_CIasin: `FST qword [esp]` of ST(0), no stack arguments,
// plain RET; the result replaces ST(0).
static void crt_CIunary(X86 *c, const char *name, double (*fn)(double)) {
    if (fempty(c, 0)) {
        static unsigned logged = 0;
        if (logged++ < 4)
            LOGW("%s called without an x87 operand", name);
        return;
    }
    double x = ST(c, 0);
    fdrop(c);
    fpush(c, fn(x));
}
void crt_CIacos(X86 *c) { crt_CIunary(c, "_CIacos", [](double x) { return std::acos(x); }); }
void crt_CIasin(X86 *c) { crt_CIunary(c, "_CIasin", [](double x) { return std::asin(x); }); }

// int sscanf(const char *s, const char *fmt, ...). Covers whitespace,
// literals, %d/%i, %f/%e/%g (float *) and %lf (double *). Returns the number
// of assignments, or -1 if the input ends before the first conversion.
void crt_sscanf(X86 *c) {
    uint32_t s = arg(c, 0), fmt = arg(c, 1);
    GuestVaCursor32 out{c->r[R_ESP] + 12u};
    auto peek = [](uint32_t a) -> uint8_t { return a && gm_valid(a, 1) ? rd8(a) : 0; };
    int assigned = 0;
    bool converted_any = false;
    for (;;) {
        uint8_t f = peek(fmt);
        if (!f)
            break;
        if (crt_space(f)) {
            while (crt_space(peek(fmt)))
                ++fmt;
            while (crt_space(peek(s)))
                ++s;
            continue;
        }
        if (f != '%') {
            if (peek(s) != f)
                break;
            ++s, ++fmt;
            continue;
        }
        uint8_t conv = peek(++fmt);
        uint32_t width = 0;
        while (conv >= '0' && conv <= '9') {
            width = width * 10 + (conv - '0');
            conv = peek(++fmt);
        }
        bool wide = false;
        if (conv == 'l') {
            wide = true;
            conv = peek(++fmt);
        }
        ++fmt;
        while (crt_space(peek(s)))
            ++s;
        if (!peek(s)) {
            if (!converted_any)
                assigned = -1;
            break;
        }
        if (conv == 's') {
            uint32_t dst = out.u32();
            uint32_t n = 0;
            while (peek(s) && !crt_space(peek(s)) && (!width || n < width)) {
                if (dst && gm_valid(dst + n, 1))
                    wr8(dst + n, peek(s));
                ++s, ++n;
            }
            if (dst && gm_valid(dst + n, 1))
                wr8(dst + n, 0);
            ++assigned;
            converted_any = true;
            continue;
        }
        if (width) {
            static unsigned logged = 0;
            if (logged++ < 4)
                LOGW("sscanf: field width on %%%c is outside the supported subset", conv);
            break;
        }
        std::string text;
        uint32_t at = s;
        auto take_digits = [&]() {
            size_t before = text.size();
            while (peek(at) >= '0' && peek(at) <= '9')
                text.push_back((char)peek(at++));
            return text.size() > before;
        };
        if (peek(at) == '-' || peek(at) == '+')
            text.push_back((char)peek(at++));
        if (conv == 'd' || conv == 'i') {
            if (!take_digits())
                break;
            uint32_t dst = out.u32();
            if (dst && gm_valid(dst, 4))
                wr32(dst, (uint32_t)(int32_t)strtol(text.c_str(), nullptr, 10));
        } else if (conv == 'f' || conv == 'e' || conv == 'g') {
            bool digits = take_digits();
            if (peek(at) == '.') {
                text.push_back((char)peek(at++));
                digits |= take_digits();
            }
            if (!digits)
                break;
            if ((peek(at) | 0x20) == 'e') {
                uint32_t mark = at;
                std::string saved = text;
                text.push_back('e');
                ++at;
                if (peek(at) == '-' || peek(at) == '+')
                    text.push_back((char)peek(at++));
                if (!take_digits())
                    text = saved, at = mark;
            }
            double value = strtod(text.c_str(), nullptr);
            uint32_t dst = out.u32();
            if (wide && dst && gm_valid(dst, 8)) {
                uint64_t bits;
                memcpy(&bits, &value, 8);
                wr32(dst, (uint32_t)bits);
                wr32(dst + 4, (uint32_t)(bits >> 32));
            } else if (!wide && dst && gm_valid(dst, 4)) {
                float narrow = (float)value;
                uint32_t bits;
                memcpy(&bits, &narrow, 4);
                wr32(dst, bits);
            }
        } else {
            static unsigned logged = 0;
            if (logged++ < 4)
                LOGW("sscanf: conversion %%%c is outside the supported subset", conv);
            break;
        }
        s = at;
        ++assigned;
        converted_any = true;
    }
    set_eax(c, (uint32_t)assigned);
}

// int __cdecl sprintf(char *dst, const char *fmt, ...). The variadic values
// sit on the guest stack immediately after the two fixed arguments.
void crt_sprintf(X86 *c) {
    GuestVaCursor32 args{c->r[4] + 12u};
    crt_write_formatted(c, arg(c, 0), arg(c, 1), &args);
}

// void __cdecl _exit(int status). MSVCR71 terminates the process here and does
// not return, and it does not run atexit handlers. Scarface calls this after
// the "No DirectSound" dialog; returning let the guest keep walking.
void crt_exit(X86 *c) {
    guest_terminate_process(arg(c, 0));
}

// void __cdecl qsort(void *base, size_t count, size_t width, int (__cdecl *compare)(...)).
// The comparator is a guest address. guest_call builds the cdecl frame and
// returns EAX. Records are swapped as raw guest bytes so an 8-byte
// pointer-plus-float stays paired. Near-equal ordering is not specified.
void crt_qsort(X86 *c) {
    uint32_t base = arg(c, 0);
    uint32_t count = arg(c, 1);
    uint32_t width = arg(c, 2);
    uint32_t compare = arg(c, 3);
    if (count < 2 || !width || !compare)
        return;
    log_once("qsort-first", "qsort: base=%08x count=%u width=%u compare=%08x", base, count, width,
             compare);
    if (width > 0x100000u || count > 0xffffffffu / width || !gm_valid(base, count * width) ||
        !gm_valid(compare, 1)) {
        log_once("qsort-range", "qsort: guest range or comparator is not mapped");
        return;
    }
    auto at = [&](uint32_t index) { return base + index * width; };
    auto before = [&](uint32_t left, uint32_t right) {
        return (int32_t)guest_call(c, compare, at(left), at(right)) < 0;
    };
    auto swap_records = [&](uint32_t left, uint32_t right) {
        if (left == right)
            return;
        uint32_t a = at(left), b = at(right);
        uint32_t off = 0;
        for (; off + 4 <= width; off += 4) {
            uint32_t tmp = rd32(a + off);
            wr32(a + off, rd32(b + off));
            wr32(b + off, tmp);
        }
        for (; off < width; ++off) {
            uint8_t tmp = rd8(a + off);
            wr8(a + off, rd8(b + off));
            wr8(b + off, tmp);
        }
    };
    auto sift = [&](uint32_t root, uint32_t end) {
        for (;;) {
            uint64_t child64 = (uint64_t)root * 2u + 1u;
            if (child64 >= end)
                break;
            uint32_t child = (uint32_t)child64;
            if (child + 1u < end && before(child, child + 1u))
                ++child;
            if (!before(root, child))
                break;
            swap_records(root, child);
            root = child;
        }
    };
    for (uint32_t start = count / 2u; start-- > 0;)
        sift(start, count);
    for (uint32_t end = count; end-- > 1;) {
        swap_records(0, end);
        sift(0, end);
    }
}

void crt_initterm(X86 *c) {
    uint32_t begin = arg(c, 0), end = arg(c, 1);
    set_eax(c, 0);
    if (begin >= end || end - begin > 0x100000u || (begin & 3u) || (end & 3u))
        return;
    for (uint32_t slot = begin; slot < end; slot += 4) {
        if (!gm_valid(slot, 4))
            return;
        uint32_t fn = rd32(slot);
        if (!fn)
            continue;
        set_eax(c, guest_call(c, fn));
    }
}
// msvcr71 malloc(0) still returns a distinct heap pointer. free(NULL) does
// nothing. realloc(NULL, n) is malloc, and realloc(p, 0) frees and returns NULL.
void crt_malloc(X86 *c) {
    uint32_t size = arg(c, 0);
    set_eax(c, heap_alloc(size ? size : 1, false));
}
void crt_free(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p && heap_owns(p))
        heap_free(p);
    set_eax(c, 0);
}
void crt_realloc(X86 *c) {
    uint32_t p = arg(c, 0), size = arg(c, 1);
    if (!p) {
        set_eax(c, heap_alloc(size ? size : 1, false));
        return;
    }
    if (!size) {
        if (heap_owns(p))
            heap_free(p);
        set_eax(c, 0);
        return;
    }
    set_eax(c, heap_realloc(p, size, false));
}
// MSVC x86 RTTI uses absolute guest pointers. The vfptr[-1] slot points at a
// CompleteObjectLocator; its class hierarchy lists BaseClassDescriptors with
// PMDs describing each base subobject. Keep this in guest address space: host
// C++ RTTI cannot describe objects built by the recompiled x86 program.
bool rtti_type_equal(uint32_t a, uint32_t b) {
    if (!a || !b || !gm_valid(a, 9) || !gm_valid(b, 9))
        return false;
    if (a == b)
        return true;
    // TypeDescriptor's decorated name begins after its two pointer fields.
    for (uint32_t i = 0; i < 256; ++i) {
        if (!gm_valid(a + 8u + i, 1) || !gm_valid(b + 8u + i, 1))
            return false;
        uint8_t ca = rd8(a + 8u + i), cb = rd8(b + 8u + i);
        if (ca != cb)
            return false;
        if (!ca)
            return true;
    }
    return false;
}

bool rtti_base_address(uint32_t complete, uint32_t base, uint32_t *out) {
    if (!base || !gm_valid(base, 24))
        return false;
    int32_t member = int32_t(rd32(base + 8));
    int32_t vbptr = int32_t(rd32(base + 12));
    // pdisp == -1 is a non-virtual base: address = complete + mdisp. That is
    // the MSVC 7.1 encoding for both single inheritance and non-virtual
    // multiple inheritance (CH_MULTINH). Virtual bases store a vbptr
    // displacement here; do not invent a vbtable walk.
    if (vbptr != -1)
        return false;
    int64_t address = int64_t(complete) + member;
    if (address < 0 || address >= GUEST_SIZE)
        return false;
    *out = uint32_t(address);
    return true;
}

void crt_dynamic_cast(X86 *c) {
    uint32_t object = arg(c, 0), delta = arg(c, 1);
    uint32_t source = arg(c, 2), target = arg(c, 3);
    bool reference = arg(c, 4) != 0;
    set_eax(c, 0);
    if (!object)
        return;
    uint64_t vfptr_slot = uint64_t(object) + delta;
    if (vfptr_slot > UINT32_MAX || !gm_valid(uint32_t(vfptr_slot), 4))
        return;
    uint32_t vtable = rd32(uint32_t(vfptr_slot));
    if (vtable < 4 || !gm_valid(vtable - 4, 4))
        return;
    uint32_t locator = rd32(vtable - 4);
    if (!locator || !gm_valid(locator, 20) || rd32(locator) != 0)
        return;
    uint32_t offset = rd32(locator + 4);
    if (vfptr_slot < offset)
        return;
    uint32_t complete = uint32_t(vfptr_slot - offset);
    uint32_t hierarchy = rd32(locator + 16);
    if (!hierarchy || !gm_valid(hierarchy, 16))
        return;
    uint32_t attributes = rd32(hierarchy + 4), count = rd32(hierarchy + 8);
    uint32_t bases = rd32(hierarchy + 12);
    if (!count || count > 1024 || !bases || !gm_valid(bases, count * 4))
        return;
    // CH_VIRTINH (bit 1) needs vbtable displacement. CH_MULTINH (bit 0) is
    // only extra bases with nonzero mdisp; the walk below already applies it.
    if (attributes & 2u) {
        static unsigned vi_logs;
        if (vi_logs < 8u) {
            LOGW("msvcr71!__RTDynamicCast: unsupported virtual-inheritance attributes %08x",
                 attributes);
            ++vi_logs;
        }
        return;
    }
    bool found_source = false, found_target = false, target_ambiguous = false;
    uint32_t answer = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t base = rd32(bases + i * 4);
        if (!base || !gm_valid(base, 24))
            return;
        uint32_t address = 0;
        if (!rtti_base_address(complete, base, &address))
            continue;
        uint32_t type = rd32(base);
        uint32_t battr = rd32(base + 20);
        if (rtti_type_equal(type, source) && address == object)
            found_source = true;
        if (!rtti_type_equal(type, target))
            continue;
        // BCD_AMBIGUOUS: pointer casts must return NULL rather than pick a base.
        if (battr & 2u) {
            target_ambiguous = true;
            continue;
        }
        if (found_target && answer != address)
            target_ambiguous = true;
        else {
            answer = address;
            found_target = true;
        }
    }
    if (found_source && found_target && !target_ambiguous) {
        set_eax(c, answer);
        return;
    }
    if (reference)
        LOGW("msvcr71!__RTDynamicCast: failed reference cast needs bad_cast");
}
void crt_memset(X86 *c) {
    uint32_t dst = arg(c, 0), size = arg(c, 2);
    if (gm_valid(dst, size) && size)
        memset(g_mem + dst, int(arg(c, 1)), size);
    set_eax(c, dst);
}
// All supported per-user settings locations use the same Documents guest path;
// the file overlay decides where that path resides in the writable profile.
void folder_path(X86 *c) {
    uint32_t csidl = arg(c, 1), out = arg(c, 4);
    switch (csidl & 0xff) {
    case 5:
    case 26:
    case 28:
    case 35:
        break;
    default:
        set_eax(c, 0x80070057u);
        return;
    }
    if (!out || !gm_valid(out, 520)) {
        set_eax(c, 0x80070057u);
        return;
    }
    gm_put_wstr(out, shell_folder_guest_path(5, (csidl & 0x8000) != 0), 260);
    set_eax(c, 0);
}

const ImportShim shims[] = {
    {"DWMAPI.dll", "DwmIsCompositionEnabled", 1, composition_enabled},
    {"DWMAPI.dll", "DwmExtendFrameIntoClientArea", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "IsThemeActive", 0, yes},
    {"UXTHEME.dll", "IsAppThemed", 0, yes},
    {"UXTHEME.dll", "OpenThemeData", 2, zero},
    {"UXTHEME.dll", "GetWindowTheme", 1, zero},
    {"UXTHEME.dll", "CloseThemeData", 1, zero},
    {"UXTHEME.dll", "SetWindowTheme", 3, zero},
    {"UXTHEME.dll", "EnableThemeDialogTexture", 2, zero},
    {"UXTHEME.dll", "IsThemeDialogTextureEnabled", 1, zero},
    {"UXTHEME.dll", "EnableTheming", 1, zero},
    {"UXTHEME.dll", "GetThemeAppProperties", 0, theme_app_properties},
    {"UXTHEME.dll", "SetThemeAppProperties", 1, zero},
    {"UXTHEME.dll", "IsThemePartDefined", 3, zero},
    {"UXTHEME.dll", "IsThemeBackgroundPartiallyTransparent", 3, zero},
    {"UXTHEME.dll", "GetThemeSysBool", 2, zero},
    {"UXTHEME.dll", "GetThemeSysColor", 2, theme_sys_color},
    {"UXTHEME.dll", "GetThemeSysColorBrush", 2, theme_sys_color_brush},
    {"UXTHEME.dll", "GetThemeSysSize", 2, theme_sys_size},
    {"UXTHEME.dll", "GetThemeSysFont", 3, e_handle},
    {"UXTHEME.dll", "GetThemeSysString", 4, e_handle},
    {"UXTHEME.dll", "GetThemeSysInt", 3, e_handle},
    {"UXTHEME.dll", "DrawThemeBackground", 6, e_handle},
    {"UXTHEME.dll", "DrawThemeText", 9, draw_theme_text},
    {"UXTHEME.dll", "DrawThemeTextEx", 9, draw_theme_text_ex},
    {"UXTHEME.dll", "OpenThemeDataForDpi", 3, zero},
    {"UXTHEME.dll", "BeginBufferedPaint", 5, begin_buffered_paint},
    {"UXTHEME.dll", "EndBufferedPaint", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintSetAlpha", 3, buffered_paint_unavailable},
    {"UXTHEME.dll", "BeginBufferedAnimation", 8, begin_buffered_animation},
    {"UXTHEME.dll", "EndBufferedAnimation", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintRenderAnimation", 2, zero},
    {"UXTHEME.dll", "BufferedPaintStopAllAnimations", 1, zero},
    {"UXTHEME.dll", "GetThemeBackgroundContentRect", 6, e_handle},
    {"UXTHEME.dll", "GetThemeBackgroundExtent", 6, e_handle},
    {"UXTHEME.dll", "GetThemePartSize", 7, e_handle},
    {"UXTHEME.dll", "GetThemeTextExtent", 9, e_handle},
    {"UXTHEME.dll", "GetThemeTextMetrics", 5, e_handle},
    {"UXTHEME.dll", "GetThemeBackgroundRegion", 6, e_handle},
    {"UXTHEME.dll", "HitTestThemeBackground", 10, e_handle},
    {"UXTHEME.dll", "DrawThemeEdge", 8, e_handle},
    {"UXTHEME.dll", "DrawThemeIcon", 7, e_handle},
    {"UXTHEME.dll", "GetThemeColor", 5, e_handle},
    {"UXTHEME.dll", "GetThemeMetric", 6, e_handle},
    {"UXTHEME.dll", "GetThemeString", 6, e_handle},
    {"UXTHEME.dll", "GetThemeBool", 5, e_handle},
    {"UXTHEME.dll", "GetThemeInt", 5, e_handle},
    {"UXTHEME.dll", "GetThemeEnumValue", 5, e_handle},
    {"UXTHEME.dll", "GetThemePosition", 5, e_handle},
    {"UXTHEME.dll", "GetThemeFont", 6, e_handle},
    {"UXTHEME.dll", "GetThemeRect", 5, e_handle},
    {"UXTHEME.dll", "GetThemeMargins", 7, e_handle},
    {"UXTHEME.dll", "GetThemeIntList", 5, e_handle},
    {"UXTHEME.dll", "GetThemePropertyOrigin", 5, e_handle},
    {"UXTHEME.dll", "GetThemeFilename", 6, e_handle},
    {"UXTHEME.dll", "GetCurrentThemeName", 6, e_handle},
    {"UXTHEME.dll", "GetThemeDocumentationProperty", 4, e_handle},
    {"UXTHEME.dll", "DrawThemeParentBackground", 3, draw_parent_background},
    {"UXTHEME.dll", "BufferedPaintInit", 0, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintUnInit", 0, zero},
    {"WTSAPI32.dll", "WTSRegisterSessionNotification", 2, session_service_unavailable},
    {"WTSAPI32.dll", "WTSUnRegisterSessionNotification", 1, session_service_unavailable},
    {"WINSPOOL.DRV", "EnumPrintersW", 7, enum_printers},
    {"WINSPOOL.DRV", "GetDefaultPrinterW", 2, default_printer},
    {"WINSPOOL.DRV", "OpenPrinterW", 3, open_printer},
    {"WINSPOOL.DRV", "ClosePrinter", 1, yes},
    {"WINSPOOL.DRV", "DocumentPropertiesW", 6, document_properties},
    {"NETAPI32.dll", "NetWkstaGetInfo", 3, workstation_info},
    {"NETAPI32.dll", "NetApiBufferFree", 1, zero},
    {"msvcrt.dll", "memcpy", ARGC_CDECL, crt_memcpy},
    {"msvcrt.dll", "memmove", ARGC_CDECL, crt_memmove},
    {"msvcr71.dll", "memmove", ARGC_CDECL, crt_memmove},
    {"msvcrt.dll", "memset", ARGC_CDECL, crt_memset},
    {"msvcrt.dll", "toupper", ARGC_CDECL, crt_toupper},
    {"msvcr71.dll", "toupper", ARGC_CDECL, crt_toupper},
    {"msvcrt.dll", "strncpy", ARGC_CDECL, crt_strncpy},
    {"msvcr71.dll", "strncpy", ARGC_CDECL, crt_strncpy},
    {"msvcrt.dll", "strrchr", ARGC_CDECL, crt_strrchr},
    {"msvcr71.dll", "strrchr", ARGC_CDECL, crt_strrchr},
    {"msvcrt.dll", "strchr", ARGC_CDECL, crt_strchr},
    {"msvcr71.dll", "strchr", ARGC_CDECL, crt_strchr},
    {"msvcrt.dll", "strstr", ARGC_CDECL, crt_strstr},
    {"msvcr71.dll", "strstr", ARGC_CDECL, crt_strstr},
    {"msvcrt.dll", "strncmp", ARGC_CDECL, crt_strncmp},
    {"msvcr71.dll", "strncmp", ARGC_CDECL, crt_strncmp},
    {"msvcrt.dll", "_strcmpi", ARGC_CDECL, crt_strcmpi},
    {"msvcr71.dll", "_strcmpi", ARGC_CDECL, crt_strcmpi},
    {"msvcrt.dll", "strtok", ARGC_CDECL, crt_strtok},
    {"msvcr71.dll", "strtok", ARGC_CDECL, crt_strtok},
    {"msvcrt.dll", "vsprintf", ARGC_CDECL, crt_vsprintf},
    {"msvcr71.dll", "vsprintf", ARGC_CDECL, crt_vsprintf},
    {"msvcrt.dll", "_vsnprintf", ARGC_CDECL, crt_vsnprintf},
    {"msvcr71.dll", "_vsnprintf", ARGC_CDECL, crt_vsnprintf},
    {"msvcrt.dll", "_strlwr", ARGC_CDECL, crt_strlwr},
    {"msvcr71.dll", "_strlwr", ARGC_CDECL, crt_strlwr},
    {"msvcrt.dll", "_strupr", ARGC_CDECL, crt_strupr},
    {"msvcr71.dll", "_strupr", ARGC_CDECL, crt_strupr},
    {"msvcrt.dll", "strcspn", ARGC_CDECL, crt_strcspn},
    {"msvcr71.dll", "strcspn", ARGC_CDECL, crt_strcspn},
    {"msvcrt.dll", "atoi", ARGC_CDECL, crt_atoi},
    {"msvcr71.dll", "atoi", ARGC_CDECL, crt_atoi},
    {"msvcrt.dll", "strtol", ARGC_CDECL, crt_strtol},
    {"msvcr71.dll", "strtol", ARGC_CDECL, crt_strtol},
    {"msvcrt.dll", "bsearch", ARGC_CDECL, crt_bsearch},
    {"msvcr71.dll", "bsearch", ARGC_CDECL, crt_bsearch},
    {"msvcrt.dll", "ceil", ARGC_CDECL, crt_ceil},
    {"msvcr71.dll", "ceil", ARGC_CDECL, crt_ceil},
    {"msvcrt.dll", "floor", ARGC_CDECL, crt_floor},
    {"msvcr71.dll", "floor", ARGC_CDECL, crt_floor},
    {"msvcrt.dll", "atof", ARGC_CDECL, crt_atof},
    {"msvcr71.dll", "atof", ARGC_CDECL, crt_atof},
    {"msvcrt.dll", "strtod", ARGC_CDECL, crt_strtod},
    {"msvcr71.dll", "strtod", ARGC_CDECL, crt_strtod},
    {"msvcrt.dll", "_CIacos", 0, crt_CIacos},
    {"msvcr71.dll", "_CIacos", 0, crt_CIacos},
    {"msvcrt.dll", "_CIasin", 0, crt_CIasin},
    {"msvcr71.dll", "_CIasin", 0, crt_CIasin},
    {"msvcrt.dll", "sscanf", ARGC_CDECL, crt_sscanf},
    {"msvcr71.dll", "sscanf", ARGC_CDECL, crt_sscanf},
    {"msvcrt.dll", "__p__fmode", 0, crt_p_fmode},
    {"msvcr71.dll", "__p__fmode", 0, crt_p_fmode},
    {"msvcrt.dll", "__p__commode", 0, crt_p_commode},
    {"msvcr71.dll", "__p__commode", 0, crt_p_commode},
    {"msvcrt.dll", "qsort", ARGC_CDECL, crt_qsort},
    {"msvcr71.dll", "qsort", ARGC_CDECL, crt_qsort},
    {"msvcrt.dll", "srand", ARGC_CDECL, crt_srand},
    {"msvcr71.dll", "srand", ARGC_CDECL, crt_srand},
    {"msvcrt.dll", "rand", ARGC_CDECL, crt_rand},
    {"msvcr71.dll", "rand", ARGC_CDECL, crt_rand},
    {"msvcrt.dll", "sprintf", ARGC_CDECL, crt_sprintf},
    {"msvcr71.dll", "sprintf", ARGC_CDECL, crt_sprintf},
    {"msvcrt.dll", "isspace", ARGC_CDECL, crt_isspace},
    {"msvcr71.dll", "isspace", ARGC_CDECL, crt_isspace},
    {"msvcrt.dll", "isalpha", ARGC_CDECL, crt_isalpha},
    {"msvcr71.dll", "isalpha", ARGC_CDECL, crt_isalpha},
    {"msvcrt.dll", "isdigit", ARGC_CDECL, crt_isdigit},
    {"msvcr71.dll", "isdigit", ARGC_CDECL, crt_isdigit},
    {"msvcrt.dll", "isalnum", ARGC_CDECL, crt_isalnum},
    {"msvcr71.dll", "isalnum", ARGC_CDECL, crt_isalnum},
    {"msvcrt.dll", "_CIfmod", 0, crt_CIfmod},
    {"msvcr71.dll", "_CIfmod", 0, crt_CIfmod},
    {"msvcrt.dll", "_CIpow", 0, crt_CIpow},
    {"msvcr71.dll", "_CIpow", 0, crt_CIpow},
    {"msvcrt.dll", "_initterm", ARGC_CDECL, crt_initterm},
    {"msvcr71.dll", "_initterm", ARGC_CDECL, crt_initterm},
    {"msvcrt.dll", "_exit", ARGC_CDECL, crt_exit},
    {"msvcr71.dll", "_exit", ARGC_CDECL, crt_exit},
    {"msvcrt.dll", "malloc", ARGC_CDECL, crt_malloc},
    {"msvcr71.dll", "malloc", ARGC_CDECL, crt_malloc},
    {"msvcrt.dll", "free", ARGC_CDECL, crt_free},
    {"msvcr71.dll", "free", ARGC_CDECL, crt_free},
    {"msvcrt.dll", "realloc", ARGC_CDECL, crt_realloc},
    {"msvcr71.dll", "realloc", ARGC_CDECL, crt_realloc},
    {"msvcr71.dll", "__RTDynamicCast", ARGC_CDECL, crt_dynamic_cast},
    {"SHFOLDER.dll", "SHGetFolderPathW", 5, folder_path},
    {"SHELL32.dll", "Shell_NotifyIconW", 2, yes},
    {"SHELL32.dll", "SHAppBarMessage", 2, zero},
    {"SHELL32.dll", "ShellExecuteW", 6, zero},
    {"URLMON.dll", "URLDownloadToFileW", 5, nullptr},
    // stdcall, four arguments. A null function returns FALSE and does not
    // write the output buffer; the argument count is what the caller needs.
    {"WININET.dll", "InternetQueryOptionA", 4, nullptr},
    {"vorbisfile.dll", "ov_open_callbacks", ARGC_CDECL, vorbis_ov_open_callbacks},
    {"vorbisfile.dll", "ov_info", ARGC_CDECL, vorbis_ov_info},
    {"vorbisfile.dll", "ov_pcm_total", ARGC_CDECL, vorbis_ov_pcm_total},
    {"vorbisfile.dll", "ov_pcm_seek", ARGC_CDECL, vorbis_ov_pcm_seek},
    {"vorbisfile.dll", "ov_read", ARGC_CDECL, vorbis_ov_read},
    {"vorbisfile.dll", "ov_clear", ARGC_CDECL, vorbis_ov_clear},
};
} // namespace
void misc_dlls_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}
