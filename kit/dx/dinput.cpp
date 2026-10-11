// dinput.cpp - DirectInput: the DirectInput object plus mouse and keyboard
// devices, fed from host_input_state(), and the joystick device whose
// behaviour lives in dinput_joystick.cpp.
//
// GetDeviceState reports the host's current state. GetDeviceData replays the
// buffered events the shim derives by diffing successive host states, which is
// what a real buffered device delivers and what a guest that asks for relative
// mouse motion needs.
#include "com.h"
#include "dinput_joystick.h"
#include "dx.h"

#include <mutex>
#include "host_api.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../runtime/native_seam.h"

#include <string.h>
#include <stdio.h>
#include <iterator>
#include <algorithm>
#include "../platform/os.h"

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

static const uint8_t IID_IDirectInputA_[16] =
    IID_BYTES(0x89521360, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t IID_IDirectInput2A_[16] =
    IID_BYTES(0x5944E662, 0xAA8A, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t IID_IDirectInputDeviceA_[16] =
    IID_BYTES(0x5944E680, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t IID_IDirectInputDevice2A_[16] =
    IID_BYTES(0x5944E682, 0xC92E, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_SysMouse_[16] =
    IID_BYTES(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_SysKeyboard_[16] =
    IID_BYTES(0x6F1D2B61, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_Key_[16] =
    IID_BYTES(0x55728220, 0xD33C, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_XAxis_[16] =
    IID_BYTES(0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_YAxis_[16] =
    IID_BYTES(0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_ZAxis_[16] =
    IID_BYTES(0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_Button_[16] =
    IID_BYTES(0xA36D02F0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

namespace {

// DIPROP_* are GUIDs cast from small integers, so the pointer value itself is
// the property id: MAKEDIPROP(1) is (GUID*)1.
const uint32_t DIPROP_BUFFERSIZE = 1;
const uint32_t DIPROP_AXISMODE = 2;
const uint32_t DIPROP_GRANULARITY = 3;
const uint32_t DIPROP_RANGE = 4;

// The byte offsets a mouse event reports in dwOfs, matching DIMOUSESTATE.
const uint32_t DIMOFS_X = 0;
const uint32_t DIMOFS_Y = 4;
const uint32_t DIMOFS_Z = 8;
const uint32_t DIMOFS_BUTTON0 = 12;

uint32_t g_scratch = 0, g_scratch_size = 0;
uint32_t scratch(uint32_t n) {
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

bool is_joystick(const ComObj *d) {
    return d->dev_type == DIDEVTYPE_JOYSTICK;
}

// ---------------------------------------------------------------------------
// Notification handles, kept apart from the COM objects on purpose.
//
// dinput_host_input_changed runs on the host's own thread - the AppKit event
// thread, in the windowed host - while a guest thread may be running guest
// code. It must not walk the COM objects to find the registered events,
// because those belong to the baton holder. So the handles live in this small
// list under their own mutex: the guest thread writes it from
// SetEventNotification, the host thread reads it, and nothing else is shared.
// ---------------------------------------------------------------------------
std::mutex g_notify_m;
struct Notify {
    uint32_t obj_id;
    uint32_t event;
};
std::vector<Notify> g_notify;

void dinput_set_notify(uint32_t obj_id, uint32_t event) {
    std::lock_guard<std::mutex> lock(g_notify_m);
    for (auto it = g_notify.begin(); it != g_notify.end(); ++it) {
        if (it->obj_id != obj_id)
            continue;
        if (event)
            it->event = event;
        else
            g_notify.erase(it);
        return;
    }
    if (event)
        g_notify.push_back(Notify{obj_id, event});
}

ComObj *this_dinput(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_DINPUT) ? o : nullptr;
}
ComObj *this_device(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_DIDEVICE) ? o : nullptr;
}

// A buffered event, packed so ComObj needs no extra type: the object offset
// in the top byte (a scan code is 0..255 and a mouse offset is 0..19) and the
// value in the low 24 bits. Axis deltas are signed and are clamped to that
// range rather than allowed to wrap into the wrong sign.
uint32_t pack_event(uint32_t ofs, int32_t data) {
    if (data > 0x7fffff)
        data = 0x7fffff;
    if (data < -0x800000)
        data = -0x800000;
    return ((ofs & 0xffu) << 24) | ((uint32_t)data & 0x00ffffffu);
}
uint32_t event_ofs(uint32_t e) {
    return e >> 24;
}
uint32_t event_data(uint32_t e) {
    return e & 0x00ffffffu;
}
// Sign-extends a 24-bit packed value back to a full dword. The inner shift
// must happen on the signed type, so the parentheses are load-bearing.
int32_t event_data_signed(uint32_t e) {
    return ((int32_t)(e << 8)) >> 8;
}

// One read of the host state serves every device.
//
// host_input_state CONSUMES the mouse deltas: the host clears them once read,
// because they are motion since the last read rather than a position. So a
// device that reads them and does not want them destroys them. With the
// keyboard and the mouse both woken by the same notification, and the keyboard
// usually polling first, every scrap of mouse motion was being eaten by the
// keyboard's poll and thrown away - which looks exactly like a mouse whose
// buttons work and whose movement does not, because buttons are a level the
// host keeps reporting and motion is a delta reported once.
//
// So the deltas are read once and accumulated here, and only the mouse takes
// them, at which point the accumulator is cleared. Everything else about the
// host state is a level and can be read by anyone.
struct HostInput {
    uint8_t keys[256] = {0};
    uint8_t buttons[8] = {0};
    int32_t acc_dx = 0, acc_dy = 0, acc_dz = 0;
};
HostInput g_host_in;

struct InputCensus {
    uint32_t create_a = 0;
    uint32_t create_a_version = 0;
    uint32_t create_a_hr = 0;
    uint32_t create8 = 0;
    uint32_t create_dev = 0;
    uint32_t create_dev_fail = 0;
    uint32_t mouse = 0;
    uint32_t keyboard = 0;
    uint32_t joystick = 0;
    uint32_t acquire_ok = 0;
    uint32_t acquire_fail = 0;
    uint32_t getstate = 0;
    uint32_t getstate_notacq = 0;
    uint32_t getstate_key_down = 0;
    uint32_t getstate_mouse_dx = 0;
    uint32_t getdata = 0;
    uint32_t getdata_events = 0;
    uint32_t last_unknown_guid[4] = {};
};
InputCensus g_census;

void refresh_host_input() {
    HostInputState in;
    memset(&in, 0, sizeof in);
    host_input_state(&in);
    memcpy(g_host_in.keys, in.keys, sizeof g_host_in.keys);
    memcpy(g_host_in.buttons, in.mouse_buttons, sizeof g_host_in.buttons);
    g_host_in.acc_dx += in.mouse_dx;
    g_host_in.acc_dy += in.mouse_dy;
    g_host_in.acc_dz += in.mouse_dz;
}

// Appends whatever changed since the last poll. This is the only place device
// state advances, so GetDeviceState and GetDeviceData always agree about what
// "now" is.
void poll_device(ComObj *d) {
    // The joystick reads the pad's own state and edge queue on demand, and
    // must not take the mouse's accumulated motion.
    if (is_joystick(d))
        return;
    refresh_host_input();

    if (d->dev_type == DIDEVTYPE_KEYBOARD) {
        for (uint32_t k = 0; k < 256; ++k) {
            uint8_t now = g_host_in.keys[k] & 0x80u;
            if (now != (d->last_keys[k] & 0x80u)) {
                if (d->buffer_size)
                    d->events.push_back(pack_event(k, now ? 0x80 : 0));
                d->last_keys[k] = now;
            }
        }
        return;
    }

    // Mouse. The axes are relative in DirectInput's default mode. The mouse is
    // the only device entitled to the accumulated motion, so it takes it and
    // clears the accumulator.
    int32_t dx = g_host_in.acc_dx, dy = g_host_in.acc_dy, dz = g_host_in.acc_dz;
    g_host_in.acc_dx = g_host_in.acc_dy = g_host_in.acc_dz = 0;

    host_input_pointer_correction(&dx, &dy);

    if (dx && d->buffer_size)
        d->events.push_back(pack_event(DIMOFS_X, dx));
    if (dy && d->buffer_size)
        d->events.push_back(pack_event(DIMOFS_Y, dy));
    if (dz && d->buffer_size)
        d->events.push_back(pack_event(DIMOFS_Z, dz));
    // The immediate state reports motion since the last GetDeviceState, so it
    // accumulates here and is cleared there. Overwriting would report only the
    // last poll's motion and lose everything a GetDeviceData poll had already
    // taken in between.
    d->last_x += dx;
    d->last_y += dy;
    d->last_z += dz;
    for (uint32_t b = 0; b < 8; ++b) {
        uint8_t now = g_host_in.buttons[b] & 0x80u;
        if (now != (d->last_buttons[b] & 0x80u)) {
            if (d->buffer_size)
                d->events.push_back(pack_event(DIMOFS_BUTTON0 + b, now ? 0x80 : 0));
            d->last_buttons[b] = now;
        }
    }
    // A buffer that overflows drops the oldest events and the next
    // GetDeviceData has to report DI_BUFFEROVERFLOW; capping here keeps the
    // vector bounded when the guest never drains it.
    if (d->buffer_size && d->events.size() > d->buffer_size) {
        size_t drop = d->events.size() - d->buffer_size;
        d->events.erase(d->events.begin(), d->events.begin() + (ptrdiff_t)drop);
        log_once("dinput.overflow", "dinput: the %u-event buffer overflowed; oldest events dropped",
                 d->buffer_size);
    }
}

// ===========================================================================
// IDirectInputDeviceA
// ===========================================================================
void Device_GetCapabilities(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t out = arg(c, 1);
    if (!d || !out || !gm_valid(out, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    uint32_t size = rd32(out);
    if (size != DIDEVCAPS_SIZE && size != DIDEVCAPS_DX3_SIZE) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!gm_valid(out, size)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    gm_zero(out + 4, size - 4);
    if (is_joystick(d)) {
        com_ret(c, joy_get_capabilities(d, out, size));
        return;
    }
    wr32(out + DIDC_OFF_dwFlags, DIDC_ATTACHED);
    wr32(out + DIDC_OFF_dwDevType, d->dev_type);
    if (size >= DIDEVCAPS_DX3_SIZE) {
        wr32(out + DIDC_OFF_dwAxes, d->dev_type == DIDEVTYPE_MOUSE ? 3u : 0u);
        wr32(out + DIDC_OFF_dwButtons, d->dev_type == DIDEVTYPE_MOUSE ? 4u : 256u);
        wr32(out + DIDC_OFF_dwPOVs, 0);
    }
    com_ret(c, DI_OK);
}

// Scarface constructs its device formats from these object callbacks.
void Device_EnumObjects(X86 *c) {
    ComObj *d = this_device(c);
    if (d && is_joystick(d)) {
        com_ret(c, joy_enum_objects(c, d, arg(c, 1), arg(c, 2), arg(c, 3)));
        return;
    }
    if (d && d->dev_type == DIDEVTYPE_KEYBOARD) {
        uint32_t cb = arg(c, 1), ref = arg(c, 2), flags = arg(c, 3);
        if (!cb) {
            com_ret(c, DIERR_INVALIDPARAM);
            return;
        }
        if (!(flags & 0x1F000000u) &&
            (!(flags & DIDFT_TYPEMASK) || (flags & DIDFT_PSHBUTTON))) {
            const uint32_t size = d->di_wide ? DIDEVICEOBJECTINSTANCEW_SIZE
                                              : DIDEVICEOBJECTINSTANCEA_SIZE;
            for (uint32_t key = 0; key < 256; ++key) {
                uint32_t a = scratch(size);
                if (!a) {
                    com_ret(c, E_OUTOFMEMORY);
                    return;
                }
                wr32(a, size);
                memcpy(gm_ptr(a + DIDOI_OFF_guidType), GUID_Key_, 16);
                wr32(a + DIDOI_OFF_dwOfs, key);
                wr32(a + DIDOI_OFF_dwType, DIDFT_PSHBUTTON | (key << 8));
                char name[32];
                snprintf(name, sizeof name, "Key %u", key);
                if (d->di_wide)
                    di_put_wide(a + DIDOI_OFF_tszName, name, DIDOI_NAME_CHARS);
                else
                    gm_put_str(a + DIDOI_OFF_tszName, name, DIDOI_NAME_CHARS);
                if (guest_call(c, cb, a, ref) != 1 /* DIENUM_CONTINUE */)
                    break;
            }
        }
        com_ret(c, DI_OK);
        return;
    }
    if (d && d->dev_type == DIDEVTYPE_MOUSE) {
        uint32_t cb = arg(c, 1), ref = arg(c, 2), flags = arg(c, 3);
        if (!cb) {
            com_ret(c, DIERR_INVALIDPARAM);
            return;
        }
        const uint8_t *axis_guids[3] = {GUID_XAxis_, GUID_YAxis_, GUID_ZAxis_};
        const char *axis_names[3] = {"X Axis", "Y Axis", "Z Axis"};
        const uint32_t size = d->di_wide ? DIDEVICEOBJECTINSTANCEW_SIZE
                                          : DIDEVICEOBJECTINSTANCEA_SIZE;
        for (uint32_t i = 0; i < 7; ++i) {
            bool axis = i < 3;
            uint32_t instance = axis ? i : i - 3;
            uint32_t type = (axis ? DIDFT_RELAXIS : DIDFT_PSHBUTTON) | (instance << 8);
            if ((flags & 0x1F000000u) ||
                ((flags & DIDFT_TYPEMASK) && !(flags & type & DIDFT_TYPEMASK)))
                continue;
            uint32_t a = scratch(size);
            if (!a) {
                com_ret(c, E_OUTOFMEMORY);
                return;
            }
            wr32(a, size);
            memcpy(gm_ptr(a + DIDOI_OFF_guidType), axis ? axis_guids[i] : GUID_Button_, 16);
            wr32(a + DIDOI_OFF_dwOfs, axis ? i * 4 : DIMOFS_BUTTON0 + instance);
            wr32(a + DIDOI_OFF_dwType, type);
            char name[32];
            if (axis)
                snprintf(name, sizeof name, "%s", axis_names[i]);
            else
                snprintf(name, sizeof name, "Button %u", instance);
            if (d->di_wide)
                di_put_wide(a + DIDOI_OFF_tszName, name, DIDOI_NAME_CHARS);
            else
                gm_put_str(a + DIDOI_OFF_tszName, name, DIDOI_NAME_CHARS);
            if (guest_call(c, cb, a, ref) != 1 /* DIENUM_CONTINUE */)
                break;
        }
        com_ret(c, DI_OK);
        return;
    }
    log_once("dx.Device_EnumObjects", "dx: Device_EnumObjects is not implemented; returning DI_OK");
    com_ret(c, DI_OK);
}

void Device_GetProperty(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t prop = arg(c, 1), ph = arg(c, 2);
    // DIPROPDWORD is a 16-byte header plus dwData at offset 16, so the whole
    // 20 bytes must be addressable before dwData is touched.
    if (!d || !ph || !gm_valid(ph, DIPROPDWORD_SIZE)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    uint32_t hr = DI_OK;
    if (is_joystick(d) && joy_get_property(d, prop, ph, &hr)) {
        com_ret(c, hr);
        return;
    }
    if (prop == DIPROP_BUFFERSIZE) {
        wr32(ph + DIPROPDWORD_OFF_dwData, d->buffer_size);
        com_ret(c, DI_OK);
        return;
    }
    if (prop == DIPROP_AXISMODE) {
        wr32(ph + DIPROPDWORD_OFF_dwData, d->axis_mode_absolute);
        com_ret(c, DI_OK);
        return;
    }
    if (prop == DIPROP_GRANULARITY) {
        wr32(ph + DIPROPDWORD_OFF_dwData, 1);
        com_ret(c, DI_OK);
        return;
    }
    log_once("dinput.getprop", "dinput: GetProperty %u is not supported", prop);
    com_ret(c, DIERR_UNSUPPORTED);
}

void Device_SetProperty(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t prop = arg(c, 1), ph = arg(c, 2);
    if (!d || !ph || !gm_valid(ph, DIPROPDWORD_SIZE)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    uint32_t hr = DI_OK;
    if (is_joystick(d) && joy_set_property(d, prop, ph, &hr)) {
        com_ret(c, hr);
        return;
    }
    if (prop == DIPROP_BUFFERSIZE) {
        uint32_t n = rd32(ph + DIPROPDWORD_OFF_dwData);
        if (n > 4096)
            n = 4096;
        d->buffer_size = n;
        d->events.clear();
        LOGV("dinput: buffer size %u on device type %u", n, d->dev_type);
        com_ret(c, DI_OK);
        return;
    }
    if (prop == DIPROP_AXISMODE) {
        d->axis_mode_absolute = rd32(ph + DIPROPDWORD_OFF_dwData);
        com_ret(c, DI_OK);
        return;
    }
    if (prop == DIPROP_RANGE || prop == DIPROP_GRANULARITY) {
        com_ret(c, DI_PROPNOEFFECT);
        return;
    }
    log_once("dinput.setprop", "dinput: SetProperty %u is not supported", prop);
    com_ret(c, DIERR_UNSUPPORTED);
}

void Device_Acquire(X86 *c) {
    ComObj *d = this_device(c);
    if (!d) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!d->data_format_size) {
        g_census.acquire_fail++;
        com_ret(c, DIERR_NOTINITIALIZED);
        return;
    }
    if (d->acquired) {
        com_ret(c, S_FALSE);
        return;
    }
    d->acquired = true;
    g_census.acquire_ok++;
    if (g_census.acquire_ok <= 4)
        LOGW("dinput: Acquire %s ok (n=%u)",
             d->dev_type == DIDEVTYPE_MOUSE      ? "mouse"
             : d->dev_type == DIDEVTYPE_KEYBOARD ? "keyboard"
                                                 : "joystick",
             g_census.acquire_ok);
    // Acquiring resets the event stream, so the first read after acquisition
    // reports the state now rather than a backlog from before.
    d->events.clear();
    memset(d->last_keys, 0, sizeof d->last_keys);
    memset(d->last_buttons, 0, sizeof d->last_buttons);
    if (is_joystick(d))
        joy_acquired(d);
    com_ret(c, DI_OK);
}

void Device_Unacquire(X86 *c) {
    ComObj *d = this_device(c);
    if (!d) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!d->acquired) {
        com_ret(c, S_FALSE);
        return;
    }
    d->acquired = false;
    d->events.clear();
    com_ret(c, DI_OK);
}

// Return the current keyboard or mouse state using the acquired device format.
// Guest buffer sizes and device acquisition are checked before writing state.
void Device_GetDeviceState(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t size = arg(c, 1), out = arg(c, 2);
    if (!d || !out || !size || !gm_fits(out, size)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!d->acquired) {
        g_census.getstate_notacq++;
        com_ret(c, DIERR_NOTACQUIRED);
        return;
    }
    if (is_joystick(d)) {
        com_ret(c, joy_get_device_state(d, size, out));
        return;
    }

    // poll_device is the only reader of the host state: the deltas it
    // consumes are cleared by the host, so a second read here would report
    // zero motion and lose this frame's movement.
    poll_device(d);
    g_census.getstate++;

    if (d->dev_type == DIDEVTYPE_KEYBOARD) {
        if (!d->key_format.empty()) {
            gm_zero(out, size);
            for (const KeyFormatSlot &slot : d->key_format)
                if (slot.guest_ofs < size)
                    wr8(out + slot.guest_ofs, d->last_keys[slot.scan_code]);
            com_ret(c, DI_OK);
            return;
        }
        // The keyboard format is one byte per DIK_ scan code.
        if (size > 256)
            size = 256;
        uint32_t down = 0;
        for (uint32_t k = 0; k < size; ++k) {
            wr8(out + k, d->last_keys[k]);
            if (d->last_keys[k] & 0x80u)
                down++;
        }
        if (down) {
            g_census.getstate_key_down++;
            if (g_census.getstate_key_down <= 8)
                LOGW("dinput: GetDeviceState keyboard %u keys down (poll #%u)", down,
                     g_census.getstate);
        } else if (g_census.getstate <= 3)
            LOGW("dinput: GetDeviceState keyboard idle (poll #%u size=%u)", g_census.getstate,
                 size);
        com_ret(c, DI_OK);
        return;
    }
    if (d->mouse_format.empty() && size != DIMOUSESTATE_SIZE && size != DIMOUSESTATE2_SIZE) {
        log_once("dinput.mousefmt",
                 "dinput: GetDeviceState with a %u-byte mouse structure; "
                 "DIMOUSESTATE is %u and DIMOUSESTATE2 is %u",
                 size, (uint32_t)DIMOUSESTATE_SIZE, (uint32_t)DIMOUSESTATE2_SIZE);
    }
    gm_zero(out, size);
    if (!d->mouse_format.empty()) {
        for (const ComObj::MouseFormatSlot &slot : d->mouse_format) {
            if (slot.source_ofs < DIMOFS_BUTTON0) {
                if (slot.guest_ofs + 4 <= size) {
                    int32_t value = slot.source_ofs == DIMOFS_X ? d->last_x
                                    : slot.source_ofs == DIMOFS_Y ? d->last_y : d->last_z;
                    wr32(out + slot.guest_ofs, (uint32_t)value);
                }
            } else if (slot.guest_ofs < size) {
                wr8(out + slot.guest_ofs, d->last_buttons[slot.source_ofs - DIMOFS_BUTTON0]);
            }
        }
        d->last_x = d->last_y = d->last_z = 0;
        com_ret(c, DI_OK);
        return;
    }
    // The deltas poll_device just consumed are this frame's motion.
    if (size >= 12) {
        if (d->last_x || d->last_y) {
            g_census.getstate_mouse_dx++;
            if (g_census.getstate_mouse_dx <= 8)
                LOGW("dinput: GetDeviceState mouse dx=%d dy=%d (n=%u)", d->last_x, d->last_y,
                     g_census.getstate_mouse_dx);
        } else if (g_census.getstate <= 3)
            LOGW("dinput: GetDeviceState mouse idle (poll #%u size=%u)", g_census.getstate, size);
        wr32(out + DIMS_OFF_lX, (uint32_t)d->last_x);
        wr32(out + DIMS_OFF_lY, (uint32_t)d->last_y);
        wr32(out + DIMS_OFF_lZ, (uint32_t)d->last_z);
    }
    uint32_t nbuttons = size >= DIMOUSESTATE2_SIZE ? 8u : 4u;
    for (uint32_t b = 0; b < nbuttons && DIMS_OFF_rgbButtons + b < size; ++b)
        wr8(out + DIMS_OFF_rgbButtons + b, d->last_buttons[b]);
    // Relative axes report motion since the previous call, so reading them is
    // what clears them. The buttons are a level and stay.
    d->last_x = d->last_y = d->last_z = 0;
    com_ret(c, DI_OK);
}

// Read buffered device events, honoring peek and buffer-overflow behavior.
// Translate native input into the guest event layout without exposing host pointers.
void Device_GetDeviceData(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t objsize = arg(c, 1);
    uint32_t out = arg(c, 2);
    uint32_t inout = arg(c, 3);
    uint32_t flags = arg(c, 4);
    if (!d || !inout || !gm_valid(inout, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    // The game asks for the DirectX 5 record (16 bytes); the 20-byte DirectX 8
    // form is accepted too. Events are emitted at whichever stride the caller
    // asked for, because it walks the buffer by that stride itself.
    if (objsize != DIDEVICEOBJECTDATA_SIZE && objsize != DIDEVICEOBJECTDATA_DX8_SIZE) {
        log_once("dinput.objsize",
                 "dinput: GetDeviceData with a %u-byte event record; "
                 "DirectInput 5 uses %u and DirectInput 8 uses %u",
                 objsize, (uint32_t)DIDEVICEOBJECTDATA_SIZE, (uint32_t)DIDEVICEOBJECTDATA_DX8_SIZE);
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!d->acquired) {
        com_ret(c, DIERR_NOTACQUIRED);
        return;
    }
    if (!d->buffer_size) {
        // A device with no buffer is not a buffered device; DirectInput
        // reports this rather than silently returning nothing.
        log_once("dinput.nobuffer", "dinput: GetDeviceData on a device with DIPROP_BUFFERSIZE 0");
        wr32(inout, 0);
        com_ret(c, DIERR_NOTACQUIRED);
        return;
    }
    if (is_joystick(d)) {
        com_ret(c, joy_get_device_data(d, objsize, out, inout, flags));
        return;
    }

    poll_device(d);
    g_census.getdata++;
    uint32_t want = rd32(inout);
    // DIGDD_PEEK = 1: report without consuming.
    bool peek = (flags & 1u) != 0;
    uint32_t have = (uint32_t)d->events.size();
    uint32_t n = want < have ? want : have;
    if (out) {
        if (!gm_fits_n(out, n, objsize)) {
            com_ret(c, DIERR_INVALIDPARAM);
            return;
        }
        uint32_t now = host_millis();
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t e = d->events[i];
            uint32_t a = out + i * objsize;
            if (objsize >= DIDEVICEOBJECTDATA_DX8_SIZE)
                wr32(a + DIDOD_OFF_uAppData, 0);
            uint32_t ofs = event_ofs(e);
            uint32_t guest_ofs = ofs;
            if (d->dev_type == DIDEVTYPE_KEYBOARD && !d->key_format.empty()) {
                auto it = std::find_if(d->key_format.begin(), d->key_format.end(),
                    [ofs](const KeyFormatSlot &slot) { return slot.scan_code == ofs; });
                if (it != d->key_format.end())
                    guest_ofs = it->guest_ofs;
            } else if (d->dev_type == DIDEVTYPE_MOUSE && !d->mouse_format.empty()) {
                auto it = std::find_if(d->mouse_format.begin(), d->mouse_format.end(),
                    [ofs](const ComObj::MouseFormatSlot &slot) { return slot.source_ofs == ofs; });
                if (it != d->mouse_format.end())
                    guest_ofs = it->guest_ofs;
            }
            wr32(a + DIDOD_OFF_dwOfs, guest_ofs);
            // dwData is a full dword; a signed axis delta was packed into 24
            // bits, so sign-extend it back.
            uint32_t data = (ofs == DIMOFS_X || ofs == DIMOFS_Y || ofs == DIMOFS_Z)
                                ? (uint32_t)event_data_signed(e)
                                : event_data(e);
            wr32(a + DIDOD_OFF_dwData, data);
            wr32(a + DIDOD_OFF_dwTimeStamp, now);
            wr32(a + DIDOD_OFF_dwSequence, ++d->sequence);
            static const bool trace = recomp_env("TRACE_POINTER") != nullptr;
            if (trace && ofs >= DIMS_OFF_rgbButtons)
                fprintf(stderr, "[dinput-button] ofs %u data %08x seq %u\n", ofs, data,
                        d->sequence);
            // Axis deltas matter most while the right button is held: that is
            // the camera mode, which reads them and nothing else.
            if (trace && ofs < DIMS_OFF_rgbButtons && (d->last_buttons[1] & 0x80u))
                fprintf(stderr, "[dinput-axis] ofs %u delta %d seq %u (right held)\n", ofs,
                        int32_t(data), d->sequence);
        }
    }
    wr32(inout, n);
    if (n) {
        g_census.getdata_events += n;
        if (g_census.getdata_events <= 16)
            LOGW("dinput: GetDeviceData %s delivered %u event(s)",
                 d->dev_type == DIDEVTYPE_MOUSE      ? "mouse"
                 : d->dev_type == DIDEVTYPE_KEYBOARD ? "keyboard"
                                                     : "joystick",
                 n);
    }
    if (!peek && n)
        d->events.erase(d->events.begin(), d->events.begin() + (ptrdiff_t)n);
    com_ret(c, DI_OK);
}

void Device_SetDataFormat(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t df = arg(c, 1);
    if (!d || !df || !gm_valid(df, 24)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (is_joystick(d)) {
        com_ret(c, joy_set_data_format(d, df));
        return;
    }
    // DIDATAFORMAT: dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf.
    uint32_t data_size = rd32(df + 12);
    if (!data_size) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (d->dev_type == DIDEVTYPE_KEYBOARD) {
        uint32_t count = rd32(df + DIDF_OFF_dwNumObjs);
        uint32_t records = rd32(df + DIDF_OFF_rgodf);
        uint32_t stride = rd32(df + DIDF_OFF_dwObjSize);
        if (data_size > 4096 || count > 256 ||
            (count && (stride < DIOBJECTDATAFORMAT_SIZE || !gm_fits_n(records, count, stride)))) {
            com_ret(c, DIERR_INVALIDPARAM);
            return;
        }
        std::vector<KeyFormatSlot> format;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t record = records + i * stride;
            uint32_t ofs = rd32(record + DIODF_OFF_dwOfs);
            uint32_t type = rd32(record + DIODF_OFF_dwType);
            uint32_t key = didft_instance(type);
            if (!(type & DIDFT_BUTTON) || key >= 256 || ofs >= data_size) {
                com_ret(c, DIERR_INVALIDPARAM);
                return;
            }
            format.push_back({key, ofs});
        }
        d->key_format = std::move(format);
    } else if (d->dev_type == DIDEVTYPE_MOUSE) {
        uint32_t count = rd32(df + DIDF_OFF_dwNumObjs);
        uint32_t records = rd32(df + DIDF_OFF_rgodf);
        uint32_t stride = rd32(df + DIDF_OFF_dwObjSize);
        if (data_size > 4096 || count > 11 ||
            (count && (stride < DIOBJECTDATAFORMAT_SIZE || !gm_fits_n(records, count, stride)))) {
            com_ret(c, DIERR_INVALIDPARAM);
            return;
        }
        std::vector<ComObj::MouseFormatSlot> format;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t record = records + i * stride;
            uint32_t ofs = rd32(record + DIODF_OFF_dwOfs);
            uint32_t type = rd32(record + DIODF_OFF_dwType);
            uint32_t instance = didft_instance(type);
            bool axis = (type & DIDFT_AXIS) != 0;
            bool button = (type & DIDFT_BUTTON) != 0;
            if (axis == button ||
                (axis && (instance >= 3 || data_size < 4 || ofs > data_size - 4)) ||
                (button && (instance >= 8 || ofs >= data_size))) {
                com_ret(c, DIERR_INVALIDPARAM);
                return;
            }
            format.push_back({axis ? instance * 4 : DIMOFS_BUTTON0 + instance, ofs});
        }
        d->mouse_format = std::move(format);
    }
    d->data_format_size = data_size;
    LOGV("dinput: data format of %u bytes on device type %u", data_size, d->dev_type);
    com_ret(c, DI_OK);
}

// The event DirectInput signals when the device has data for the guest. The
// game's two service threads at 0052c880 and 0052ceda register one and then
// wait on it forever, so a shim that accepts the call and never signals
// anything is a shim that makes the mouse and keyboard dead.
//
// The handle is also kept in a small list of its own, guarded by a mutex,
// because the host signals from its own thread and must not walk the COM
// objects: those belong to whichever guest thread holds the baton.
void Device_SetEventNotification(X86 *c) {
    ComObj *d = this_device(c);
    if (!d) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    uint32_t h = arg(c, 1);
    if (h == 0xffffffffu)
        h = 0; // INVALID_HANDLE_VALUE clears it
    d->notify_event = h;
    dinput_set_notify(d->id, h);
    com_ret(c, DI_OK);
}

void Device_SetCooperativeLevel(X86 *c) {
    ComObj *d = this_device(c);
    if (!d) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    d->di_coop = arg(c, 2);
    com_ret(c, DI_OK);
}

// GetObjectInfo(pdidoi, dwObj, dwHow).
void Device_GetObjectInfo(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (d && is_joystick(d)) {
        com_ret(c, joy_get_object_info(d, out, arg(c, 2), arg(c, 3)));
        return;
    }
    log_once("dinput.objectinfo", "dinput: GetObjectInfo is not supported");
    com_ret(c, DIERR_UNSUPPORTED);
}

void Device_GetDeviceInfo(X86 *c) {
    ComObj *d = this_device(c);
    uint32_t out = arg(c, 1);
    if (!d || !out || !gm_valid(out, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    // DIDEVICEINSTANCEA: dwSize, guidInstance, guidProduct, dwDevType,
    // tszInstanceName[260], tszProductName[260], guidFFDriver, wUsagePage,
    // wUsage. Only the size the caller declared is written.
    uint32_t size = rd32(out);
    // DIDEVICEINSTANCEA is 580 bytes; the W record, with its two names as
    // 260 UTF-16 units each, is 1100.
    if (size < 8 || size > 1100 || !gm_valid(out, size)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    gm_zero(out + 4, size - 4);
    if (is_joystick(d)) {
        joy_write_device_instance(out, size, d->di_wide, d->di_version);
        com_ret(c, DI_OK);
        return;
    }
    bool mouse = d->dev_type == DIDEVTYPE_MOUSE;
    const uint8_t *g = mouse ? GUID_SysMouse_ : GUID_SysKeyboard_;
    if (size >= 4 + 16)
        memcpy(gm_ptr(out + 4), g, 16);
    if (size >= 4 + 32)
        memcpy(gm_ptr(out + 20), g, 16);
    if (size >= 40)
        wr32(out + 36, d->dev_type);
    // DIDEVICEINSTANCEW carries the two names as 260 UTF-16 units each, so
    // its fields past dwDevType sit at 40 and 560 and the record is 1100 bytes.
    if (d->di_wide) {
        for (uint32_t at : {40u, 560u})
            if (size >= at + 520)
                di_put_wide(out + at, mouse ? "Mouse" : "Keyboard", 260);
        com_ret(c, DI_OK);
        return;
    }
    if (size >= 40 + 260)
        gm_put_str(out + 40, mouse ? "Mouse" : "Keyboard", 260);
    if (size >= 300 + 260)
        gm_put_str(out + 300, mouse ? "Mouse" : "Keyboard", 260);
    com_ret(c, DI_OK);
}

DX_STUB(Device_RunControlPanel, DIERR_UNSUPPORTED)
DX_STUB(Device_Initialize, DI_OK)

// --- IDirectInputDevice2A additions (force feedback and polling)
DX_STUB(Device_CreateEffect, DIERR_UNSUPPORTED)
DX_STUB(Device_EnumEffects, DI_OK)
DX_STUB(Device_GetEffectInfo, DIERR_UNSUPPORTED)
DX_STUB(Device_GetForceFeedbackState, DIERR_UNSUPPORTED)
DX_STUB(Device_SendForceFeedbackCommand, DIERR_UNSUPPORTED)
DX_STUB(Device_EnumCreatedEffectObjects, DI_OK)
DX_STUB(Device_Escape, DIERR_UNSUPPORTED)

void Device_Poll(X86 *c) {
    ComObj *d = this_device(c);
    if (!d) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    if (!d->acquired) {
        com_ret(c, DIERR_NOTACQUIRED);
        return;
    }
    // A joystick has nothing to poll (poll_device skips it) and answers
    // DI_OK, not DI_NOEFFECT, so a game that checks for DI_OK keeps reading.
    poll_device(d);
    com_ret(c, DI_OK);
}

DX_STUB(Device_SendDeviceData, DIERR_UNSUPPORTED)

#define DIDEVICE_COMMON_SLOTS                                                                      \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetCapabilities", 2, Device_GetCapabilities},               \
        {"EnumObjects", 4, Device_EnumObjects}, {"GetProperty", 3, Device_GetProperty},            \
        {"SetProperty", 3, Device_SetProperty}, {"Acquire", 1, Device_Acquire},                    \
        {"Unacquire", 1, Device_Unacquire}, {"GetDeviceState", 3, Device_GetDeviceState},          \
        {"GetDeviceData", 5, Device_GetDeviceData}, {"SetDataFormat", 2, Device_SetDataFormat},    \
        {"SetEventNotification", 2, Device_SetEventNotification},                                  \
        {"SetCooperativeLevel", 3, Device_SetCooperativeLevel},                                    \
        {"GetObjectInfo", 4, Device_GetObjectInfo}, {"GetDeviceInfo", 2, Device_GetDeviceInfo},    \
        {"RunControlPanel", 3, Device_RunControlPanel}, {                                          \
        "Initialize", 4, Device_Initialize                                                         \
    }

const ComMethod g_didevice[] = {
    DIDEVICE_COMMON_SLOTS,
    // IDirectInputDevice2A continues in the same vtable. The game creates its
    // devices through IDirectInput::CreateDevice, which returns the version 1
    // interface, but a QueryInterface for the version 2 one must land on the
    // same slots for the first 18 entries and then find these.
    {"CreateEffect", 5, Device_CreateEffect},
    {"EnumEffects", 4, Device_EnumEffects},
    {"GetEffectInfo", 3, Device_GetEffectInfo},
    {"GetForceFeedbackState", 2, Device_GetForceFeedbackState},
    {"SendForceFeedbackCommand", 2, Device_SendForceFeedbackCommand},
    {"EnumCreatedEffectObjects", 4, Device_EnumCreatedEffectObjects},
    {"Escape", 2, Device_Escape},
    {"Poll", 1, Device_Poll},
    {"SendDeviceData", 5, Device_SendDeviceData},
};

// ===========================================================================
// IDirectInputA
// ===========================================================================
static void create_device(X86 *c, ComIface iface) {
    ComObj *di = this_dinput(c);
    uint32_t guid = arg(c, 1);
    uint32_t out = arg(c, 2);
    uint32_t outer = arg(c, 3);
    if (!di || !out) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    com_out_ptr(out, 0);
    if (outer) {
        com_ret(c, CLASS_E_NOAGGREGATION);
        return;
    }
    if (!guid || !gm_valid(guid, 16)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }

    uint32_t type = 0;
    if (memcmp(gm_ptr(guid), GUID_SysMouse_, 16) == 0)
        type = DIDEVTYPE_MOUSE;
    else if (memcmp(gm_ptr(guid), GUID_SysKeyboard_, 16) == 0)
        type = DIDEVTYPE_KEYBOARD;
    else if (joy_served() && joy_guid(gm_ptr(guid)))
        type = DIDEVTYPE_JOYSTICK;
    else {
        // A joystick the game did not ask the pad to serve, or any other
        // instance GUID. There is no such device here, and the documented
        // answer lets the game fall back cleanly.
        g_census.create_dev_fail++;
        memcpy(g_census.last_unknown_guid, gm_ptr(guid), 16);
        LOGW("dinput: CreateDevice unknown GUID %08x-%08x-%08x-%08x DIERR_DEVICENOTREG",
             rd32(guid), rd32(guid + 4), rd32(guid + 8), rd32(guid + 12));
        com_ret(c, DIERR_DEVICENOTREG);
        return;
    }

    ComObj *d = com_new(K_DIDEVICE);
    d->dev_type = type;
    d->di_wide = di->di_wide;
    d->di_version = di->di_version;
    uint32_t view = com_view(d, iface);
    if (!view) {
        com_release(d);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view);
    g_census.create_dev++;
    if (type == DIDEVTYPE_MOUSE)
        g_census.mouse++;
    else if (type == DIDEVTYPE_KEYBOARD)
        g_census.keyboard++;
    else
        g_census.joystick++;
    LOGW("dinput: created the %s device (n=%u)",
         type == DIDEVTYPE_MOUSE      ? "mouse"
         : type == DIDEVTYPE_KEYBOARD ? "keyboard"
                                      : "joystick",
         g_census.create_dev);
    com_ret(c, DI_OK);
}
void DI_CreateDevice(X86 *c) {
    create_device(c, IF_DINPUTDEVICE);
}

// EnumDevices(dwDevType, callback, ref, dwFlags). The mouse and keyboard
// always; the virtual pad when it is served, the filter admits a gamepad and
// the caller did not ask for force feedback. DIEDFL_ATTACHEDONLY needs no
// test: every device here is attached.
//
// `v8` says which interface asked: DirectInput 8 names the device types by
// their DI8DEVTYPE_ codes and filters by DI8DEVCLASS_, so the records the
// callback sees differ even though the devices do not.
static void enum_devices(X86 *c, bool v8) {
    uint32_t devtype = arg(c, 1);
    uint32_t cb = arg(c, 2), ref = arg(c, 3), flags = arg(c, 4);
    if (!cb) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    // DIDEVICEINSTANCEA is 4 + 16 + 16 + 4 + 260 + 260 + 16 + 4 = 580 bytes
    // in the DirectX 5 layout the game was built against.
    ComObj *di_ = this_dinput(c);
    const bool wide = di_ && di_->di_wide;
    const uint32_t INST_SIZE = wide ? 1100 : 580;
    struct Dev {
        uint32_t type;
        const uint8_t *guid;
        const char *name;
    };
    const Dev devs[] = {
        {DIDEVTYPE_MOUSE, GUID_SysMouse_, "Mouse"},
        {DIDEVTYPE_KEYBOARD, GUID_SysKeyboard_, "Keyboard"},
    };
    bool stopped = false;
    for (const Dev &d : devs) {
        // DIEDFL_ALLDEVICES is 0, and a non-zero devtype filters by type.
        if (devtype && devtype != d.type)
            continue;
        uint32_t a = scratch(INST_SIZE);
        if (!a)
            break;
        gm_zero(a, INST_SIZE);
        wr32(a, INST_SIZE);
        memcpy(gm_ptr(a + 4), d.guid, 16);
        memcpy(gm_ptr(a + 20), d.guid, 16);
        // DirectInput 8 codes: DI8DEVTYPE_MOUSE 0x12, DI8DEVTYPE_KEYBOARD 0x13,
        // each with subtype 1. The class filter values happen to be the
        // same numbers as the version 5 types compared above.
        wr32(a + 36, v8 ? (d.type == DIDEVTYPE_MOUSE ? 0x0112u : 0x0113u) : d.type);
        if (wide) {
            di_put_wide(a + 40, d.name, 260);
            di_put_wide(a + 560, d.name, 260);
        } else {
            gm_put_str(a + 40, d.name, 260);
            gm_put_str(a + 300, d.name, 260);
        }
        if (guest_call(c, cb, a, ref) != DDENUMRET_OK) {
            stopped = true;
            break;
        }
    }
    // The interface decides which set of type codes the pad is described and
    // filtered by, not only the number the object was created with: a version
    // 8 interface is a version 8 interface whatever DirectInput8Create was
    // handed.
    uint32_t version = di_ ? di_->di_version : 0;
    if (v8 && version < DIRECTINPUT_VERSION_8)
        version = DIRECTINPUT_VERSION_8;
    if (!stopped && joy_served() && joy_enum_matches(devtype, version) &&
        !(flags & DIEDFL_FORCEFEEDBACK)) {
        uint32_t a = scratch(INST_SIZE);
        if (a) {
            wr32(a, INST_SIZE);
            joy_write_device_instance(a, INST_SIZE, wide, version);
            guest_call(c, cb, a, ref);
        }
    }
    com_ret(c, DI_OK);
}
void DI_EnumDevices(X86 *c) {
    enum_devices(c, false);
}

void DI_GetDeviceStatus(X86 *c) {
    uint32_t guid = arg(c, 1);
    if (!guid || !gm_valid(guid, 16)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    bool known = memcmp(gm_ptr(guid), GUID_SysMouse_, 16) == 0 ||
                 memcmp(gm_ptr(guid), GUID_SysKeyboard_, 16) == 0 ||
                 (joy_served() && joy_guid(gm_ptr(guid)));
    com_ret(c, known ? DI_OK : S_FALSE);
}

DX_STUB(DI_RunControlPanel, DIERR_UNSUPPORTED)

void DI_Initialize(X86 *c) {
    ComObj *di = this_dinput(c);
    if (!di) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    di->di_version = arg(c, 2);
    com_ret(c, DI_OK);
}

const ComMethod g_dinput[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"CreateDevice", 4, DI_CreateDevice},
    {"EnumDevices", 5, DI_EnumDevices},
    {"GetDeviceStatus", 2, DI_GetDeviceStatus},
    {"RunControlPanel", 3, DI_RunControlPanel},
    {"Initialize", 3, DI_Initialize},
};

// ===========================================================================
// IDirectInput8A and IDirectInputDevice8A
//
// The same objects through the version 8 layouts: the version 2 interface
// plus action mapping and device configuration, and the version 7 device plus
// action maps and image info. Those additions answer as unsupported, which is
// what a game with no action-mapped controls sees on a machine without them.
// ===========================================================================
static const uint32_t DI8_UNSUPPORTED = 0x80004001u; // E_NOTIMPL

void DI8_CreateDevice(X86 *c) {
    create_device(c, IF_DINPUTDEVICE8);
}
void DI8_EnumDevices(X86 *c) {
    enum_devices(c, true);
}
void DI8_FindDevice(X86 *c) {
    com_ret(c, DIERR_DEVICENOTREG);
}
void DI8_EnumDevicesBySemantics(X86 *c) {
    com_ret(c, DI_OK); // no action-mapped devices: the callback is never called
}
void DI8_ConfigureDevices(X86 *c) {
    com_ret(c, DI8_UNSUPPORTED);
}
void Device8_Unsupported(X86 *c) {
    com_ret(c, DI8_UNSUPPORTED);
}

const ComMethod g_dinput8[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"CreateDevice", 4, DI8_CreateDevice},
    {"EnumDevices", 5, DI8_EnumDevices},
    {"GetDeviceStatus", 2, DI_GetDeviceStatus},
    {"RunControlPanel", 3, DI_RunControlPanel},
    {"Initialize", 3, DI_Initialize},
    {"FindDevice", 4, DI8_FindDevice},
    {"EnumDevicesBySemantics", 6, DI8_EnumDevicesBySemantics},
    {"ConfigureDevices", 5, DI8_ConfigureDevices},
};

const ComMethod g_didevice8[] = {
    DIDEVICE_COMMON_SLOTS,
    {"CreateEffect", 5, Device_CreateEffect},
    {"EnumEffects", 4, Device_EnumEffects},
    {"GetEffectInfo", 3, Device_GetEffectInfo},
    {"GetForceFeedbackState", 2, Device_GetForceFeedbackState},
    {"SendForceFeedbackCommand", 2, Device_SendForceFeedbackCommand},
    {"EnumCreatedEffectObjects", 4, Device_EnumCreatedEffectObjects},
    {"Escape", 2, Device_Escape},
    {"Poll", 1, Device_Poll},
    {"SendDeviceData", 5, Device_SendDeviceData},
    {"EnumEffectsInFile", 5, Device8_Unsupported},
    {"WriteEffectToFile", 5, Device8_Unsupported},
    {"BuildActionMap", 4, Device8_Unsupported},
    {"SetActionMap", 4, Device8_Unsupported},
    {"GetImageInfo", 2, Device8_Unsupported},
};

// ===========================================================================
// DINPUT.dll exports
// ===========================================================================
// DirectInputCreateA(hinst, dwVersion, lplpDirectInput, punkOuter)
// The A and W entries make the same object; what differs is the layout of
// the two structures that carry device names, which the object remembers.
// A Unicode Delphi program asks for the W entry and, refused, runs with no
// DirectInput at all - which is a mouse read some slower way.
void direct_input_create(X86 *c, uint32_t version, uint32_t out, uint32_t outer, bool wide);
void DirectInputCreateA(X86 *c) {
    direct_input_create(c, arg(c, 1), arg(c, 2), arg(c, 3), false);
}
void DirectInputCreateW(X86 *c) {
    direct_input_create(c, arg(c, 1), arg(c, 2), arg(c, 3), true);
}
// DirectInputCreateEx(hinst, version, riid, out, outer): the IID names the
// interface, and the W ones are the A ones plus one in their first dword.
void DirectInputCreateEx(X86 *c) {
    uint32_t riid = arg(c, 2), out = arg(c, 3);
    if (!riid || !gm_valid(riid, 16)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    uint32_t data1 = rd32(riid);
    static const uint32_t ansi[] = {0x89521360u, 0x5944E662u, 0x9A4CB684u}; // IDirectInput{,2,7}A
    bool known = false, wide = false;
    for (uint32_t a : ansi) {
        if (data1 == a)
            known = true;
        if (data1 == a + 1)
            known = wide = true;
    }
    if (!known) {
        if (out && gm_valid(out, 4))
            wr32(out, 0);
        com_ret(c, 0x80004002u); // E_NOINTERFACE
        return;
    }
    direct_input_create(c, arg(c, 1), out, arg(c, 4), wide);
}
void direct_input_create(X86 *c, uint32_t version, uint32_t out, uint32_t outer, bool wide) {
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    wr32(out, 0);
    if (outer) {
        com_ret(c, CLASS_E_NOAGGREGATION);
        return;
    }
    // DirectInput refuses a version newer than the runtime. This shim
    // implements the DirectX 5 interface, so anything above 0x0500 would be a
    // promise it cannot keep.
    // dinput.dll shipping with DX8 accepts DirectInputCreate*(0x0800) and
    // still returns IDirectInputA. Rejecting 0x0800 forced every caller that
    // defined DIRECTINPUT_VERSION 0x0800 onto a missing fallback.
    if (version > 0x0800u) {
        g_census.create_a++;
        g_census.create_a_version = version;
        g_census.create_a_hr = MAKE_DIHRESULT(0x2000u);
        LOGW("dinput: DirectInputCreateA for version %04x is newer than this "
             "shim implements",
             version);
        com_ret(c, MAKE_DIHRESULT(0x2000u)); // DIERR_OLDDIRECTINPUTVERSION
        return;
    }
    ComObj *di = com_new(K_DINPUT);
    di->di_version = version;
    di->di_wide = wide;
    uint32_t view = com_view(di, IF_DINPUT);
    if (!view) {
        com_release(di);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    g_census.create_a++;
    g_census.create_a_version = version;
    g_census.create_a_hr = DI_OK;
    LOGW("dinput: DirectInputCreateA(version=%04x) -> %08x", version, view);
    com_ret(c, DI_OK);
}

void DirectInput8Create(X86 *c) {
    uint32_t version = arg(c, 1);
    uint32_t out = arg(c, 3);
    uint32_t outer = arg(c, 4);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, DIERR_INVALIDPARAM);
        return;
    }
    wr32(out, 0);
    if (outer) {
        com_ret(c, CLASS_E_NOAGGREGATION);
        return;
    }
    ComObj *di = com_new(K_DINPUT);
    // Devices made through this object describe themselves with the version 8
    // type codes, so the object remembers at least version 8 whatever number
    // it was handed. Nothing else reads di_version.
    di->di_version = version < DIRECTINPUT_VERSION_8 ? DIRECTINPUT_VERSION_8 : version;
    uint32_t view = com_view(di, IF_DINPUT8);
    if (!view) {
        com_release(di);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    g_census.create8++;
    LOGW("dinput: DirectInput8Create(version=%04x) -> %08x", version, view);
    com_ret(c, DI_OK);
}

// CLSID_DirectInput8 {25E609E4-B259-11CF-BFC7-444553540000}. Radical's
// ControllerSystemDirectInput constructs this through ole32
// CoCreateInstance, then IDirectInput8::Initialize(hinst, 0x0800). The
// object is the same host record DirectInput8Create makes; it is not
// initialised until the guest calls Initialize.
static const uint8_t CLSID_DirectInput8_[16] =
    IID_BYTES(0x25E609E4, 0xB259, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

static ComObj *dinput8_com_create() {
    ComObj *di = com_new(K_DINPUT);
    di->di_version = DIRECTINPUT_VERSION_8;
    g_census.create8++;
    LOGW("dinput: CoCreateInstance(CLSID_DirectInput8) -> obj %u", di->id);
    return di;
}

const ImportShim g_dinput_exports[] = {
    {"DINPUT.dll", "DirectInputCreateA", 4, DirectInputCreateA},
    {"DINPUT.dll", "DirectInputCreateW", 4, DirectInputCreateW},
    {"DINPUT.dll", "DirectInputCreateEx", 5, DirectInputCreateEx},
    {"DINPUT8.dll", "DirectInput8Create", 5, DirectInput8Create},
};

} // namespace

extern "C" void dinput_discard_mouse_motion(uint32_t device) {
    ComObj *mouse = device ? com_this(device) : nullptr;
    if (!mouse || mouse->kind != K_DIDEVICE || mouse->dev_type != DIDEVTYPE_MOUSE)
        return;
    // A keyboard poll may already have drained the host's motion into this
    // shared accumulator; a mouse Poll may also have queued it on the device.
    g_host_in.acc_dx = g_host_in.acc_dy = 0;
    mouse->last_x = mouse->last_y = 0;
    auto &events = mouse->events;
    events.erase(std::remove_if(events.begin(), events.end(),
                                [](uint32_t event) {
                                    return event_ofs(event) == DIMOFS_X ||
                                           event_ofs(event) == DIMOFS_Y;
                                }),
                 events.end());
}

// Called by the host after it has fed new mouse or keyboard state through
// host_input_state. Signals every device that registered a notification event,
// which is what wakes the game's service threads out of their WaitForSingleObject
// so they can call GetDeviceData and see the input.
//
// It signals every registered device rather than only the one whose data
// changed, because working that out means polling, and polling reads and
// mutates guest-side device state that belongs to the baton holder. A guest
// woken with nothing to report calls GetDeviceData, gets no events and waits
// again, which costs one wakeup; a guest not woken when it should have been
// waits for ever. The asymmetry decides it.
extern "C" void dinput_host_input_changed(void) {
    std::lock_guard<std::mutex> lock(g_notify_m);
    for (const Notify &n : g_notify)
        guest_event_signal_from_host(n.event);
}

uint32_t di_scratch(uint32_t n) {
    return scratch(n);
}

void dinput_reset() {
    g_scratch = 0;
    g_scratch_size = 0;
    // The devices these handles belonged to are gone with the arena, and the
    // handles themselves name kernel objects that no longer exist. Signalling
    // one after a reset would be signalling whatever now has that number.
    g_host_in = HostInput();
    g_census = InputCensus();
    std::lock_guard<std::mutex> lock(g_notify_m);
    g_notify.clear();
}

extern "C" void dinput_log_census(void) {
    LOGW("dinput: census createA=%u ver=%04x hr=%08x create8=%u devices=%u "
         "(mouse=%u kbd=%u joy=%u fail=%u) acquire_ok=%u acquire_fail=%u "
         "GetDeviceState=%u notacq=%u key_down=%u mouse_motion=%u "
         "GetDeviceData=%u events=%u unknown_guid=%08x-%08x-%08x-%08x",
         g_census.create_a, g_census.create_a_version, g_census.create_a_hr, g_census.create8,
         g_census.create_dev, g_census.mouse, g_census.keyboard, g_census.joystick,
         g_census.create_dev_fail, g_census.acquire_ok, g_census.acquire_fail, g_census.getstate,
         g_census.getstate_notacq, g_census.getstate_key_down, g_census.getstate_mouse_dx,
         g_census.getdata, g_census.getdata_events, g_census.last_unknown_guid[0],
         g_census.last_unknown_guid[1], g_census.last_unknown_guid[2],
         g_census.last_unknown_guid[3]);
}

void dinput_register() {
    static bool done = false;
    if (done)
        return;
    done = true;

    com_define(IF_DINPUT, "DINPUT.dll", "IDirectInputA", g_dinput, std::size(g_dinput));
    com_define(IF_DINPUTDEVICE, "DINPUT.dll", "IDirectInputDeviceA", g_didevice,
               std::size(g_didevice));

    com_bind(IF_DINPUT, K_DINPUT);
    com_bind(IF_DINPUTDEVICE, K_DIDEVICE);
    com_define(IF_DINPUT8, "DINPUT8.dll", "IDirectInput8A", g_dinput8, std::size(g_dinput8));
    com_define(IF_DINPUTDEVICE8, "DINPUT8.dll", "IDirectInputDevice8A", g_didevice8,
               std::size(g_didevice8));
    com_bind(IF_DINPUT8, K_DINPUT);
    com_bind(IF_DINPUTDEVICE8, K_DIDEVICE);
    // {BF798030-483A-4DA2-AA99-5D64ED369700}, {54D41080-DC15-4833-A41B-748F73A38179}
    static const uint8_t iid8[16] =
        IID_BYTES(0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
    static const uint8_t iid_dev8[16] =
        IID_BYTES(0x54D41080, 0xDC15, 0x4833, 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79);
    com_register_iid(IF_DINPUT8, iid8);
    com_register_iid(IF_DINPUTDEVICE8, iid_dev8);

    com_register_iid(IF_DINPUT, IID_IDirectInputA_);
    com_register_iid(IF_DINPUT, IID_IDirectInput2A_);
    com_register_iid(IF_DINPUTDEVICE, IID_IDirectInputDeviceA_);
    com_register_iid(IF_DINPUTDEVICE, IID_IDirectInputDevice2A_);

    com_register_class(CLSID_DirectInput8_, "DirectInput8", IF_DINPUT8, dinput8_com_create);
    imports_register(g_dinput_exports, std::size(g_dinput_exports));
}
