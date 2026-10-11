# Touch Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the kit's fixed on-screen keypad with one data-driven controls system: JSON layouts that hold keys, PlayStation-style buttons, dpads and sticks. The pad reaches games as keys and mouse, a DirectInput joystick or XInput. Physical controllers and rumble use the same path. Phones get their own layouts and a portrait mode, and players get an in-game editor.

**Architecture:** SDL-free units live in `host/controls/`: json, layout, layout_store, router, vpad, binding, raster and editor. Each has its own unit tests in `controls_tests`. SDL and GPU glue lives in `host/controls/overlay.cpp`, `host/controls/gamepad_sdl.cpp` and `host/sdl/main.cpp`. The guest side (`dx/dinput_joystick.cpp`, `dx/xinput.cpp`) reads the pad through new weak `host_pad_*` callbacks in `dx/host_api.h`. Per-game configuration comes from `game.toml [controls]`, turned into macros by `tools/gen_game_config.py`.

**Tech Stack:** C++17, SDL3, CMake/Ninja via `tools/build.py` and `tools/test.py`, Python 3 for the config tools, Objective-C++ (iOS) and Java/JNI (Android) for haptics and orientation.

**Spec:** `docs/superpowers/specs/2026-09-17-touch-controls-design.md`

## Global Constraints

- One PR from branch `touch-controls` to `main`. Every commit must leave `.venv/bin/python tools/test.py --native` green on macOS.
- Native code is built only through `tools/build.py`, `tools/test.py`, or `cmake --build` / `ctest` on the already-configured `build/cmake/macos` tree. Never call a compiler directly (AGENTS.md).
- No `#ifdef` on the platform outside `platform/os_posix.cpp` and `platform/os_win32.cpp`. Platform differences live in `host/sdl/platform_ui_desktop.cpp` (macOS, Linux, Windows, Android) and `host/sdl/platform_ui_ios.mm`, as today. Android-only code reached from `platform_ui_desktop.cpp` goes behind the existing `SDL_GetPlatform()` runtime checks. Use whatever pattern that file already uses, and keep new code consistent with it.
- No game-specific literal in kit code. Run `.venv/bin/python tools/check_game_literals.py` before each commit that touches native code.
- Format with `.venv/bin/python tools/format.py --write` before each commit. Run `.venv/bin/python tools/check_repo.py` on staged changes.
- Environment switches are read through `recomp_env("NAME")` (reads `RECOMP_NAME`). There are no `POP_` names and no aliases.
- Commits are authored as veritr1x (already set up by the repo's git config). Commit messages carry no co-author trailers and no personal strings.
- Tests follow the kit style: a plain `main()` and a `CHECK(c)` macro that counts failures (see `host/tests/keypad_tests.cpp`), with no framework.
- `SDL_Scancode` values are spelled as integers in SDL-free code (the `KeypadScan` enum in `host/keypad_layout.h`).
- Comment major functions and non-obvious rules, matching the surrounding density.
- Update `CHANGELOG.md` for user-visible behaviour (Task 22).

**Inner loop commands** (from the worktree root `~/Documents/Tests/recomp-kit-controls`):

```bash
.venv/bin/cmake --build build/cmake/macos --target controls_tests && .venv/bin/ctest --test-dir build/cmake/macos -R controls_tests --output-on-failure
.venv/bin/python tools/test.py --native        # full native suites (needed before each commit)
.venv/bin/python tools/test.py                 # python suites
```

If `build/cmake/macos` must be reconfigured after a CMake change, `tools/test.py --native` does it.

---

## File Structure

| File | Responsibility |
|---|---|
| `host/controls/json.{h,cpp}` | A minimal JSON value type: parse and write (objects, arrays, strings, numbers, bools, null). |
| `host/controls/layout.{h,cpp}` | Layout model, parse/write, scancode names, geometry (anchors, grid groups, `stack_on`), hit test. |
| `host/controls/builtin_layouts.{h,cpp}` | Embedded JSON: `keys`, `pad`, `pad+keys` × `tablet`, `phone-landscape`, `phone-portrait`. |
| `host/controls/layout_store.{h,cpp}` | Form-factor selection, source lookup (profile → game → built-in), layout list, save/delete of user copies. |
| `host/controls/vpad.{h,cpp}` | `PadState`, button bits, stick/dpad math, merge, the shared `Vpad` (snapshot + edge queue + rumble request), `host_pad_*` strong definitions. |
| `host/controls/router.{h,cpp}` | Finger ownership, per-control state, modifier machine, toggles, on-screen pad source, auto-hide, draw generation. |
| `host/controls/binding.{h,cpp}` | Mapped-mode table parse (`k=v;…`), defaults, overrides, evaluation → `TouchAction`s and host actions. |
| `host/controls/raster.{h,cpp}` | Anti-aliased premultiplied RGBA primitives with gradients; 6x8 text. |
| `host/controls/overlay.{h,cpp}` | Draws `ControlsView` with the hud pipeline; stick knob quads. Replaces `host/keypad_overlay.*`. |
| `host/controls/editor.{h,cpp}` | SDL-free editor model: selection, drag, pinch, snap, add, delete, bind, save, reset, toolbar hit test. |
| `host/controls/gamepad_sdl.{h,cpp}` | `SDL_Gamepad` hot-plug → `Vpad` source 1; rumble out. |
| `host/controls/haptics.h` | `haptics_tap()`, `haptics_rumble(low, high, ms)`; defined per platform in `host/sdl/platform_ui_*`. |
| `host/controls/controls_host.{h,cpp}` | SDL glue called from `main.cpp`: owns the store, router, binding and editor, and publishes the view. Keeps `main.cpp` small. |
| `host/tests/controls_tests.cpp` + `host/tests/keypad_legacy_oracle.{h,cpp}` | All `controls_*` unit tests; the old keypad geometry kept as a regression oracle. |
| `mods/controls_settings.{h,cpp}` | Settings (`host.controls/*`), migration from `host.keypad/*`, F10 and Options rows. Replaces `mods/keypad_settings.*`. |
| `dx/host_api.{h,cpp}` | `HostPadState`, `host_pad_mode/state/next_event/rumble` with weak defaults. |
| `dx/dinput_joystick.{h,cpp}` | Joystick device behaviour, called from `dx/dinput.cpp`. |
| `dx/xinput.cpp` | XInput DLL exports. |
| `dx/tests/pad_tests.cpp` | DirectInput joystick + XInput tests with a fake pad host. |
| `host/present.h`, `host/present_thread.cpp` | `host_present_set_controls`, `host_present_game_rect`, portrait placement. |
| `host/sdl/main.cpp` | Routes fingers/keys/gamepads through `controls_host`; pointer mapping through the game rect. |
| `tools/game_config.py`, `tools/gen_game_config.py`, `games/stub/game.toml`, `tests/test_game_config.py` | `[controls]`. |
| `tools/build.py`, `tools/tests/test_build.py` | Bundle `layouts/`; Android orientation. |
| `host/Info-ios.plist.in`, `cmake/IosBundle.cmake`, `platform/android/app/src/main/AndroidManifest.xml.in`, `platform/android/.../RecompActivity.java` | Orientation and haptics. |

Removed by the end: `host/keypad_overlay.{h,cpp}`, `mods/keypad_settings.{h,cpp}`, the key tables and geometry in `host/keypad_layout.cpp` (the header keeps `KeypadScan`, `keypad_is_modifier` and `keypad_modifier_bit`).

---

## Phase 1: layout system and keypad port (no visible change)

### Task 1: JSON value, parser and writer

**Files:**
- Create: `host/controls/json.h`, `host/controls/json.cpp`
- Create: `host/tests/controls_tests.cpp`
- Modify: `host/CMakeLists.txt` (add the `controls_tests` target next to `keypad_tests`, lines 68–75)

**Interfaces:**
- Produces:
```cpp
namespace controls {
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o; // insertion order kept for stable output
    const Json *get(const char *key) const;       // null if absent or not an object
    double num(const char *key, double fallback) const;
    std::string str(const char *key, const char *fallback) const;
    bool boolean(const char *key, bool fallback) const;
    Json &set(const std::string &key, Json v);    // replaces or appends
    static Json number(double v); static Json string(std::string v);
    static Json boolean_value(bool v); static Json array(); static Json object();
};
bool json_parse(const std::string &text, Json *out, std::string *error); // error: "line L: message"
std::string json_write(const Json &v, int indent = 2); // numbers: integers without ".0", else %.6g
}
```

- [ ] **Step 1: Add the test target and a failing test**

`host/CMakeLists.txt`, after the `keypad_tests` block:

```cmake
add_executable(controls_tests ${HOST}/controls/json.cpp ${HOST}/tests/controls_tests.cpp)
target_include_directories(controls_tests PRIVATE ${POP_ROOT})
target_compile_options(controls_tests PRIVATE ${POP_WARN_WERROR})
pop_link_sdl(controls_tests)
pop_optimize(controls_tests 1)
pop_test_binary(controls_tests)
add_test(NAME controls_tests COMMAND controls_tests WORKING_DIRECTORY ${POP_ROOT})
set_tests_properties(controls_tests PROPERTIES LABELS nogame)
```

`host/tests/controls_tests.cpp`:

```cpp
// controls_tests.cpp - the on-screen controls: json, layouts, router, pad, binding, editor.
#include "../controls/json.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace controls;

static void test_json_round_trip() {
    Json v;
    std::string err;
    CHECK(json_parse(R"({"a": 1, "b": [true, false, null], "c": "x\"y\\né", "d": -2.5e1})",
                     &v, &err));
    CHECK(v.type == Json::Object);
    CHECK(v.num("a", 0) == 1);
    CHECK(v.get("b") && v.get("b")->a.size() == 3 && v.get("b")->a[0].b);
    CHECK(v.str("c", "") == "x\"y\\n\xc3\xa9");
    CHECK(v.num("d", 0) == -25);
    Json again;
    CHECK(json_parse(json_write(v), &again, &err));
    CHECK(json_write(again) == json_write(v));
    CHECK(json_write(Json::number(3)) == "3");
}

static void test_json_errors_name_the_line() {
    Json v;
    std::string err;
    CHECK(!json_parse("{\n\"a\": 1,\n\"b\" 2}", &v, &err));
    CHECK(err.rfind("line 3:", 0) == 0);
    CHECK(!json_parse("[1, 2", &v, &err));
    CHECK(!json_parse("{} trailing", &v, &err));
    CHECK(v.num("missing", 7) == 7); // a failed parse leaves a usable value
}

int main() {
    test_json_round_trip();
    test_json_errors_name_the_line();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("controls_tests: all passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it and confirm it fails.** `.venv/bin/python tools/test.py --native` reconfigures. Expected: a link or compile failure because `json.cpp` does not exist yet.

- [ ] **Step 3: Implement `json.h` / `json.cpp`**

A recursive-descent parser over `const char*` that tracks the line number.
- Strings handle the escapes `\" \\ \/ \b \f \n \r \t \uXXXX`, with UTF-8 output and surrogate pairs.
- Numbers go through `strtod`.
- Only whitespace may follow the value.
- On error, `*out` becomes `Null` and `*error` becomes `"line N: <what>"`.
- The writer escapes control characters and quotes, and uses 2-space indentation.
- An integer-valued number with `fabs < 1e15` is written with `%lld`, anything else with `%.6g`.

- [ ] **Step 4: Run the tests.** `.venv/bin/cmake --build build/cmake/macos --target controls_tests && .venv/bin/ctest --test-dir build/cmake/macos -R controls_tests --output-on-failure`. Expected: `controls_tests: all passed`.

- [ ] **Step 5: Format, check and commit**

```bash
.venv/bin/python tools/format.py --write && .venv/bin/python tools/test.py --native
git add host/controls/json.* host/tests/controls_tests.cpp host/CMakeLists.txt
git commit -m "Controls: a small JSON reader and writer"
```

---

### Task 2: Layout model, parsing and geometry

**Files:**
- Create: `host/controls/layout.h`, `host/controls/layout.cpp`
- Modify: `host/CMakeLists.txt` (add `layout.cpp` to `controls_tests`), `host/tests/controls_tests.cpp`

**Interfaces:**
- Consumes: `controls::Json`, `json_parse`, `json_write` (Task 1); `KeypadScan` from `host/keypad_layout.h`.
- Produces:
```cpp
namespace controls {
enum class Kind { Key, Button, Dpad, Stick, Toggle, Action };
enum class Anchor { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
enum class PadButton { Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Select, Start, Ps, Count };
enum class Form { Tablet, PhoneLandscape, PhonePortrait };
const char *form_name(Form f);             // "tablet", "phone-landscape", "phone-portrait"
const char *pad_button_name(PadButton b);  // "cross" ... "ps"
bool pad_button_from_name(const std::string &s, PadButton *out);
int scancode_from_name(const std::string &name); // "A","F5","LShift","Space","Up",... 0 if unknown
const char *scancode_name(int scancode);         // inverse; nullptr if unknown

struct Control {
    Kind kind = Kind::Key;
    Anchor anchor = Anchor::BottomLeft;
    double x = 0, y = 0, w = 0, h = 0; // points; "size" sets w = h; "radius" sets w = h = 2r
    int col = -1, row = -1, span = 1;  // inside a grid group only
    int scancode = 0;                  // Key
    std::string label;                 // drawn text; Key defaults to scancode_name
    PadButton button = PadButton::Cross; // Button
    int stick = 0;                     // Stick: 0 left, 1 right
    bool floating = true;              // Stick
    double deadzone = 0.15;            // Stick
    std::string target;                // Toggle: group id, layout name, or "next"
    std::string label_off;             // Toggle: label while its target group is hidden
    std::string stack_on;              // Toggle: sits on top of this group while it is visible
    std::string action;                // Action: "settings" | "system_keyboard" | "edit_layout"
};
struct Grid { int cols = 0, rows = 0; double key = 36, gap = 4; };
struct Group {
    std::string id;
    bool visible = true;
    bool has_grid = false;
    Grid grid;
    Anchor anchor = Anchor::BottomLeft; // grid groups only
    double x = 0, y = 0;                // grid groups only
    std::vector<Control> controls;
};
struct Layout {
    int version = 1;
    std::string name;
    double opacity = 1.0;
    double scale = 1.0;      // sizes only (not offsets); the "size" setting multiplies it
    bool safe_inset = true;  // anchors resolve inside the safe area
    std::vector<Group> groups;
};
// Unknown fields are ignored; unknown kinds are skipped and named in *warnings.
bool parse_layout(const std::string &text, Layout *out, std::string *error,
                  std::vector<std::string> *warnings = nullptr);
std::string write_layout(const Layout &l);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(double px, double py) const {
        return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
    }
    bool empty() const { return w <= 0 || h <= 0; }
};
struct Screen {
    int dw = 0, dh = 0;   // drawable pixels
    double scale = 1.0;   // drawable pixels per point
    Rect safe;            // safe area in drawable pixels
    Rect controls_area;   // portrait: the space below the game; empty otherwise
};
// The rectangle anchors resolve in: controls_area if set, else safe (if the
// layout wants it), else the whole drawable.
Rect anchor_area(const Layout &l, const Screen &s);
Rect group_rect(const Layout &l, int group, const Screen &s);  // grid: the grid box; else union of its controls
Rect control_rect(const Layout &l, int group, int control, const Screen &s);
struct Hit { int group = -1, control = -1; bool gap = false; }; // gap: inside a visible grid group, between keys
Hit hit_test(const Layout &l, const Screen &s, double px, double py);
}
```

**Geometry rules.** These reproduce the old keypad exactly. Let `px(v) = lround(v * s.scale)` and `A = anchor_area(l, s)`.

- **Grid group.**
  - `key_pt = lround(grid.key * l.scale)`
  - `pitch = lround((key_pt + grid.gap) * s.scale)`, `gap = lround(grid.gap * s.scale)`
  - The box is `pitch*cols × pitch*rows`, placed at `anchor` with offsets `px(x), px(y)`, as below.
  - A key's rect is `box.x + col*pitch + gap/2`, `box.y + row*pitch + gap/2`, `span*pitch - gap`, `pitch - gap`.
- **Non-grid control.**
  - `w = px(c.w * l.scale)`, `h = px(c.h * l.scale)`.
  - Placement by anchor, with `ox = px(x)` and `oy = px(y)`:
    - Left anchors: `x = A.x + ox`. Right anchors: `x = A.x + A.w - w - ox`. Horizontal centre: `x = A.x + (A.w - w)/2 + ox`.
    - Top anchors: `y = A.y + oy`. Bottom anchors: `y = A.y + A.h - h - oy`. Vertical centre: `y = A.y + (A.h - h)/2 + oy`.
- **`stack_on`.** When the named group is visible, the control keeps its x from the anchor rule, and its y becomes `group_rect(named).y - h`.
- **Hit test.**
  - Order:
    1. Toggle controls in any group, regardless of the group's visibility: a toggle is always shown, so a hidden group's tab can bring it back.
    2. The controls of visible groups, last group first and last control first.
    3. A point inside a visible grid group's box that hits no key returns `gap = true`.
  - Non-grid groups never claim gaps.
  - A zero-size screen hits nothing.

- [ ] **Step 1: Write the failing tests** (append to `controls_tests.cpp`, add calls in `main`, add `#include "../controls/layout.h"`):

```cpp
static const char *kTinyLayout = R"({
  "version": 1, "name": "tiny", "opacity": 0.5, "safe_inset": false, "future_field": 3,
  "groups": [
    {"id": "g", "grid": {"cols": 2, "rows": 1, "key": 36, "gap": 4}, "anchor": "bottom-right",
     "controls": [
       {"kind": "key", "scancode": "LShift", "col": 0, "row": 0},
       {"kind": "key", "scancode": "Space", "col": 1, "row": 0, "label": "SP"}]},
    {"id": "loose", "controls": [
       {"kind": "button", "button": "cross", "anchor": "top-left", "x": 10, "y": 20, "size": 50},
       {"kind": "stick", "stick": "right", "mode": "fixed", "anchor": "center", "radius": 30},
       {"kind": "hologram"},
       {"kind": "toggle", "target": "g", "label": "HIDE", "label_off": "KEYS",
        "anchor": "bottom-right", "w": 64, "h": 20, "stack_on": "g"}]}
  ]})";

static void test_layout_parse_and_write() {
    Layout l;
    std::string err;
    std::vector<std::string> warnings;
    CHECK(parse_layout(kTinyLayout, &l, &err, &warnings));
    CHECK(warnings.size() == 1 && warnings[0].find("hologram") != std::string::npos);
    CHECK(l.name == "tiny" && l.opacity == 0.5 && !l.safe_inset);
    CHECK(l.groups.size() == 2 && l.groups[0].has_grid && l.groups[0].grid.cols == 2);
    CHECK(l.groups[0].controls[0].scancode == kScanLShift);
    CHECK(l.groups[0].controls[0].label == "LShift");
    CHECK(l.groups[0].controls[1].label == "SP");
    const Control &stick = l.groups[1].controls[1];
    CHECK(stick.kind == Kind::Stick && stick.stick == 1 && !stick.floating && stick.w == 60);
    CHECK(l.groups[1].controls.size() == 3); // the unknown kind was skipped
    Layout again;
    CHECK(parse_layout(write_layout(l), &again, &err));
    CHECK(write_layout(again) == write_layout(l));
    CHECK(!parse_layout("{\"groups\": 5}", &l, &err));
    CHECK(scancode_from_name("F5") == kScanF5 && std::string(scancode_name(kScanUp)) == "Up");
    CHECK(scancode_from_name("NotAKey") == 0);
}

static Screen screen(int dw, int dh, double scale) {
    Screen s;
    s.dw = dw;
    s.dh = dh;
    s.scale = scale;
    s.safe = {0, 0, dw, dh};
    return s;
}

static void test_layout_geometry_and_hits() {
    Layout l;
    std::string err;
    CHECK(parse_layout(kTinyLayout, &l, &err));
    const Screen s = screen(2000, 1000, 2.0);
    // Grid: pitch = lround((36 + 4) * 2) = 80, box 160x80 in the bottom-right corner.
    const Rect box = group_rect(l, 0, s);
    CHECK(box.x == 1840 && box.y == 920 && box.w == 160 && box.h == 80);
    const Rect k1 = control_rect(l, 0, 1, s);
    CHECK(k1.x == 1840 + 80 + 4 && k1.y == 924 && k1.w == 72 && k1.h == 72);
    const Rect cross = control_rect(l, 1, 0, s);
    CHECK(cross.x == 20 && cross.y == 40 && cross.w == 100 && cross.h == 100);
    const Rect stick = control_rect(l, 1, 1, s);
    CHECK(stick.x == 940 && stick.y == 440 && stick.w == 120);
    // The toggle sits on the visible group, and in the corner once it is hidden.
    CHECK(control_rect(l, 1, 2, s).y == 920 - 40);
    l.groups[0].visible = false;
    CHECK(control_rect(l, 1, 2, s).y == 1000 - 40);
    Hit h = hit_test(l, s, 1990, 990);
    CHECK(h.group == 1 && h.control == 2); // the tab, even with its group hidden
    CHECK(hit_test(l, s, 1850, 950).group == -1); // hidden group: the game's
    l.groups[0].visible = true;
    h = hit_test(l, s, 1930, 960);
    CHECK(h.group == 0 && h.control == 1);
    h = hit_test(l, s, 1841, 921); // the half-gap at the box's corner
    CHECK(h.group == 0 && h.gap);
    CHECK(hit_test(l, s, 500, 900).group == -1); // non-grid groups never claim gaps
    l.scale = 40.0 / 36.0;
    CHECK(control_rect(l, 0, 0, s).w == lround((40 + 4) * 2.0) - 8);
    Screen none;
    CHECK(hit_test(l, none, 0, 0).group == -1);
}
```

- [ ] **Step 2: Run and confirm failure** (compile error: `layout.h` missing).
- [ ] **Step 3: Implement `layout.h/.cpp`** following the interface and the geometry rules.
  - Scancode names: letters, digits, `F1`–`F12`, `Return`, `Escape`, `Backspace`, `Tab`, `Space`, `Minus`, `Equals`, `LeftBracket`, `RightBracket`, `Backslash`, `Semicolon`, `Apostrophe`, `Grave`, `Comma`, `Period`, `Slash`, `Insert`, `Home`, `PageUp`, `Delete`, `End`, `PageDown`, `Right`, `Left`, `Down`, `Up`, `LCtrl`, `LShift`, `LAlt`.
  - Anchor spellings: `top-left`, `top-center`, `top-right`, `center-left`, `center`, `center-right`, `bottom-left`, `bottom-center`, `bottom-right`.
  - Stick `mode`: `"floating"` or `"fixed"`.
  - A malformed required field is a parse error that names the group and control index: a `groups` value that isn't an array, a key without a known `scancode`, a button without a known `button`, or a grid key without `col`/`row`.
  - Add `layout.cpp` to the `controls_tests` sources.
- [ ] **Step 4: Run the tests.** Expected: PASS.
- [ ] **Step 5: Format, run the full native suite, then commit:** `git commit -m "Controls: layout model, geometry and hit test"`

---

### Task 3: Built-in `keys` layout and the legacy regression oracle

**Files:**
- Create: `host/controls/builtin_layouts.h`, `host/controls/builtin_layouts.cpp`
- Create: `host/tests/keypad_legacy_oracle.h`, `host/tests/keypad_legacy_oracle.cpp`. These are copies of today's `keypad_keys`, `keypad_half_rect`, `keypad_tab_rect` and `keypad_key_rect`, renamed with a `legacy_` prefix, plus the two tables.
- Modify: `host/CMakeLists.txt`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Produces:
```cpp
namespace controls {
// The JSON text of a built-in layout, or nullptr. name: "keys" | "pad" | "pad+keys".
const char *builtin_layout(const std::string &name, Form form);
}
```
In this task only `keys`/`Tablet` exists. Other names and forms return nullptr until Tasks 11 and 17.

The `keys` tablet layout:
- `"safe_inset": false`.
- Group `left`: grid 8×5, key 36, gap 4, `anchor bottom-left`.
- Group `right`: the same grid, `anchor bottom-right`.
- Keys: the spec tables from `host/keypad_layout.cpp`, with the same labels (`Bksp`, `^`, `<`, `v`, `>`) and spans.
- Group `tabs` (no grid) with two toggles:
  - `{"kind":"toggle","target":"left","label":"HIDE","label_off":"KEYS","anchor":"bottom-left","w":64,"h":20,"stack_on":"left"}`
  - The same with `right` and `bottom-right`.
- A third toggle for layout cycling is added in Task 11, not here.

- [ ] **Step 1: Write the failing regression test**

```cpp
#include "../controls/builtin_layouts.h"
#include "keypad_legacy_oracle.h"

static void test_builtin_keys_matches_the_old_keypad() {
    Layout l;
    std::string err;
    CHECK(parse_layout(builtin_layout("keys", Form::Tablet), &l, &err));
    const double scales[] = {1.0, 2.0, 3.0};
    const int sizes_pt[] = {32, 36, 40};
    for (double sc : scales)
        for (int size = 0; size < 3; ++size) {
            l.scale = sizes_pt[size] / 36.0;
            const Screen s = screen(int(1180 * sc), int(820 * sc), sc);
            for (int side = 0; side < 2; ++side) {
                int n = 0;
                const KeypadKey *keys = legacy_keypad_keys(KeypadSide(side), &n);
                const Group &g = l.groups[side];
                CHECK(int(g.controls.size()) == n);
                for (int i = 0; i < n; ++i) {
                    const KeypadRect old =
                        legacy_keypad_key_rect(KeypadSide(side), keys[i], size, sc, s.dw, s.dh);
                    const Rect now = control_rect(l, side, i, s);
                    CHECK(now.x == old.x && now.y == old.y && now.w == old.w && now.h == old.h);
                    CHECK(g.controls[i].scancode == keys[i].scancode);
                    CHECK(g.controls[i].label == keys[i].label);
                }
                if (size == 1) { // tabs do not scale with key size in the old keypad
                    for (int shown = 0; shown < 2; ++shown) {
                        l.groups[side].visible = shown != 0;
                        const KeypadRect old = legacy_keypad_tab_rect(KeypadSide(side), shown != 0,
                                                                      size, sc, s.dw, s.dh);
                        const Rect now = control_rect(l, 2, side, s);
                        CHECK(now.x == old.x && now.y == old.y && now.w == old.w &&
                              now.h == old.h);
                    }
                    l.groups[side].visible = true;
                }
            }
        }
}
```

- [ ] **Step 2:** Run it and confirm it fails.
- [ ] **Step 3:** Write the oracle. Copy today's functions verbatim with renamed symbols. The oracle includes `host/keypad_layout.h` for `KeypadKey`/`KeypadRect`/`KeypadSide` only; those types stay in that header for now. Write `builtin_layouts.cpp` with the JSON as a raw string literal, and add both new `.cpp` files to `controls_tests`.
- [ ] **Step 4:** Run the tests. Expected: PASS for all 3 scales × 3 sizes.
- [ ] **Step 5:** Commit: `Controls: built-in keys layout, pinned to the old keypad geometry`

---

### Task 4: Layout store: form factor, sources, user copies

**Files:**
- Create: `host/controls/layout_store.h`, `host/controls/layout_store.cpp`
- Modify: `host/CMakeLists.txt`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
```cpp
namespace controls {
// Phone when the smaller drawable side, in points, is under 600; portrait when taller than wide.
Form form_for(int dw, int dh, double scale);
class LayoutStore {
  public:
    // profile_dir: <profile>/controls; game_dir: the game's bundled controls dir ("" if none).
    void set_dirs(std::string profile_dir, std::string game_dir);
    // Every layout name available: built-ins first ("pad", "keys", "pad+keys"), then any other
    // *.json names in the game dir and profile dir, sorted, without duplicates. "name.form.json"
    // contributes "name".
    std::vector<std::string> names() const;
    // First match: profile name.form, profile name, game name.form, game name, built-in.
    // A file that fails to parse is skipped; its error goes to *problem ("file: line N: ...").
    bool load(const std::string &name, Form form, Layout *out, std::string *problem) const;
    bool has_user_copy(const std::string &name, Form form) const;
    // Writes <profile>/controls/<name>.<form>.json through a temp file and rename.
    bool save_user_copy(const Layout &l, Form form, std::string *error) const;
    bool delete_user_copy(const std::string &name, Form form) const;
};
}
```

File I/O uses `std::filesystem` and `fopen`, as `mods/settings.cpp` does. The temporary file and rename go through `platform/os.h` (`os_mkstemp`, `os_fdopen`, `os_fd_fsync`, and the rename it offers; check `platform/os.h` for its exact name).

- [ ] **Step 1: Write the failing tests**
  - `form_for(2360, 1640, 2.0)` is `Tablet`, `form_for(2532, 1170, 3.0)` is `PhoneLandscape`, and `form_for(1170, 2532, 3.0)` is `PhonePortrait`.
  - With a temporary directory (`std::filesystem::temp_directory_path() / "controls_tests_<pid>"`), write `game/keys.json` holding the tiny layout (renamed `keys`), a broken `game/pad.json` (`{`) and `game/extra.phone-portrait.json`. Then check:
    - `names()` returns `{"pad","keys","pad+keys","extra"}`.
    - `load("keys", Tablet)` returns the game copy.
    - `load("pad", Tablet)` fails while no built-in `pad` exists, and `problem` names `pad.json` and `line 1`. (Task 11 changes this to fall through to the built-in; that test update belongs to Task 11.)
  - `save_user_copy` followed by `load` returns the saved copy, and `has_user_copy` is true.
  - `delete_user_copy` removes it, and `load` falls back to the game copy.
  - Clean up the temporary directory at the end.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit: `Controls: layout store with form factors and player copies`

---

### Task 5: Router: fingers, keys, modifiers, toggles

**Files:**
- Create: `host/controls/router.h`, `host/controls/router.cpp`
- Modify: `host/CMakeLists.txt` (add `router.cpp` and `${HOST}/keypad_modifiers.cpp` to `controls_tests`), `host/tests/controls_tests.cpp`

**Interfaces:**
- Consumes: `Layout`, `Screen`, `hit_test`, `control_rect` (Task 2); `KeypadModifiers` (`host/keypad_modifiers.h`); `keypad_is_modifier`, `keypad_modifier_bit` (`host/keypad_layout.h`).
- Produces:
```cpp
namespace controls {
class ControlsSink {
  public:
    virtual ~ControlsSink() = default;
    virtual void key(int scancode, bool down) = 0;
    virtual void action(const std::string &name) = 0;        // "settings", "system_keyboard", "edit_layout"
    virtual void switch_layout(const std::string &target) = 0; // a layout name or "next"
    virtual void group_visibility_changed() = 0;             // persist Layout group visibility
    virtual void tap() = 0;                                   // haptic tick on a press
};
struct ControlState {
    bool pressed = false;
    double knob_x = 0, knob_y = 0; // Stick: output in [-1, 1]
    double base_x = 0, base_y = 0; // Stick (floating): the centre, in drawable pixels; 0,0 = default
    uint8_t hat = 0;               // Dpad: 1 up, 2 right, 4 down, 8 left
};
class Router {
  public:
    void set_layout(Layout *layout);  // not owned; resets every finger (releasing what they held)
    void set_screen(const Screen &s);
    void set_enabled(bool on);        // false: hit nothing, release everything
    bool enabled() const;
    // True when the finger belongs to the controls (the caller must not give it to TouchMapper).
    bool finger_down(int64_t id, double px, double py, uint64_t now_ns, ControlsSink &sink);
    bool finger_motion(int64_t id, double px, double py, uint64_t now_ns, ControlsSink &sink);
    bool finger_up(int64_t id, uint64_t now_ns, ControlsSink &sink);
    bool finger_cancel(int64_t id, ControlsSink &sink);
    void cancel_all(ControlsSink &sink);
    bool owns(int64_t id) const;
    unsigned lit() const;                       // keypad_modifier_bit() bits
    const ControlState &state(int group, int control) const; // a static zero state when out of range
    uint32_t generation() const;                // bumps whenever anything drawn changes
    const Layout *layout() const;
};
}
```

**Behaviour** (the same as today's `main.cpp` lines 819–882 and 1036–1075, generalised):
- **Key controls:**
  - Down on a key: a modifier goes through `KeypadModifiers::press`; any other key calls `sink.key(sc, true)`.
  - Up: a modifier calls `release`; any other key calls `sink.key(sc, false)` and then `key_lifted`.
  - Cancel: a modifier calls `cancel`; any other key calls `sink.key(sc, false)` without `key_lifted`.
  - The modifier machine's events go out through `sink.key`.
- **Gap:** the finger is claimed and does nothing.
- **Toggle:**
  - On down, a `target` that names a group in the layout flips that group's `visible` and calls `group_visibility_changed()`.
  - Otherwise it calls `switch_layout(target)`.
  - Either way the finger is claimed and does nothing more.
- **Action:** on down it calls `sink.action(action)`.
- **Tap:** every press on a control (not on a gap) calls `sink.tap()`.
- **Motion** on an owned finger is ignored for keys, buttons, toggles and actions. Sticks and the dpad come in Task 9.
- **`cancel_all`:**
  - Releases every held non-modifier key.
  - Runs `KeypadModifiers::cancel_all`.
  - Clears all fingers and bumps `generation`.

- [ ] **Step 1: Write the failing tests.** Port these keypad behaviours using the `keys` built-in:
  1. **Recording sink.** A `struct Rec : ControlsSink` records calls as strings: `"k44+"`, `"k44-"`, `"a:settings"`, `"sw:next"`, `"vis"`, `"tap"`.
  2. **Space.** A finger on Space → `k44+`, `tap`. Lift → `k44-`.
  3. **Latched Shift.** A quick tap on Shift (down at t=0, up at t=100 ms) → `k225+`, and `lit() == 1`. Then a tap on A → `k4+`, `k4-`, `k225-`.
  4. **Left tab.** A finger on the left tab → `vis`, and `groups[0].visible == false`. A second tap → visible again.
  5. **Gap.** A finger in the gap at the box corner is claimed, and nothing is recorded.
  6. **Game area.** A finger in the middle of the screen is not claimed.
  7. **Cancel.** `finger_cancel` on a held A → `k4-`, and no latch is released.
  8. **Two fingers.** Shift held + A: `k225+`, `k4+`; lift A → `k4-`; lift Shift after 300 ms → `k225-`.
  9. **Disabled.** `set_enabled(false)` while A is held → `k4-`, and later hits return false.
  10. **Generation.** `generation()` changes after each of the above.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement. Keep a `std::map<int64_t, Owned>` where `Owned {int group, control; bool gap;}`, plus a `std::vector<std::vector<ControlState>>` sized from the layout.
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit: `Controls: the finger router for keys, modifiers and toggles`

---

### Task 6: Settings, migration and `[controls]` in game.toml

**Files:**
- Create: `mods/controls_settings.h`, `mods/controls_settings.cpp`
- Delete: `mods/keypad_settings.h`, `mods/keypad_settings.cpp`
- Modify: `mods/CMakeLists.txt`, `mods/settings.cpp` (add `mods_settings_stored_value`), `mods/mods_internal.h`, `mods/settings_page.cpp` (lines 15, 37, 55–58, 109–112, 250), `mods/options_menu.cpp` (lines 6, 154–180), `mods/tests/*` (every reference to `mods_keypad_*`; find them with `grep -rn keypad mods/tests`)
- Modify: `tools/game_config.py`, `tools/gen_game_config.py`, `games/stub/game.toml`, `tests/test_game_config.py`

**Interfaces:**
- Produces:
```cpp
// mods/controls_settings.h
enum ControlsRow { CONTROLS_LAYOUT_ROW, CONTROLS_SIZE_ROW, CONTROLS_OPACITY_ROW,
                   CONTROLS_HAPTICS_ROW, CONTROLS_PAD_WITH_CONTROLLER_ROW, CONTROLS_SNAP_ROW,
                   CONTROLS_EDIT_ROW, CONTROLS_ROW_COUNT };
// The host registers the layout names (LayoutStore::names()) before mods_page_init;
// default {"pad","keys","pad+keys"}. default_layout: RECOMP_CONTROLS_DEFAULT_LAYOUT.
void mods_controls_set_names(std::vector<std::string> names);
void mods_controls_refresh_names(); // re-clamp the layout row after the names changed (Task 21)
void mods_controls_init(const char *default_layout);
void mods_controls_reset();
int mods_controls_value(ControlsRow row);        // layout: index; size 0..2; opacity 20..100; others 0/1
std::string mods_controls_layout_name();          // names[index], or "" for the "hidden" choice
PopModStatus mods_controls_set(ControlsRow row, int value);
PopModStatus mods_controls_nudge(ControlsRow row, int delta); // EDIT_ROW: requests the editor
std::string mods_controls_line(ControlsRow row);
// Hidden groups of the active layout, bit i = groups[i] hidden. Not on the page.
uint32_t mods_controls_hidden_groups();
void mods_controls_set_hidden_groups(uint32_t bits);
// Set by the EDIT row; the host polls and clears it.
bool mods_controls_take_edit_request();
// mods/mods_internal.h
bool mods_settings_stored_value(const char *mod_id_slash_key, int64_t *out); // persisted, declared or not
```

**Settings** (`mod_id` `host.controls`):

| Key | Label | Range | Default |
|---|---|---|---|
| `layout` | Controls | 0..names | index of `default_layout`; the last index means Hidden |
| `size` | Controls size | 0..2 | 1 |
| `opacity` | Controls opacity | 20..100 | 70 |
| `haptics` | Button haptics | 0..1 | 1 |
| `pad_with_controller` | Pad with controller | 0..1 | 0 |
| `snap` | Editor snapping | 0..1 | 1 |
| `hidden` | *(not on the page)* | 0..0xffff | 0 |

The EDIT row isn't stored. It reads "Edit controls  >" and nudging it sets the edit request.

The `layout` value is stored as an **index** into the names list plus one Hidden slot. The page line shows the name (`Controls   keys`) or `hidden`.

**Migration:** runs in `mods_controls_init` when `host.controls/layout` has no stored value, but `host.keypad/left` or `host.keypad/right` does (read with `mods_settings_stored_value`).
- `layout` becomes the index of `keys`.
- `hidden` gets bit 0 set when `left` is 0 and bit 1 when `right` is 0.
- `size` takes the value of `host.keypad/size`.
- The `host.keypad/*` values are left in the file untouched and are no longer declared or written.

**game.toml** (`tools/game_config.py`):

```python
CONTROLS_LAYOUTS = ("pad", "keys", "pad+keys", "hidden")
STICK_MODES = ("cursor", "arrows", "wasd", "scroll", "wheel", "none")
PAD_BUTTONS = ("cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
               "select", "start", "ps")
MAPPED_DEFAULTS = {"left_stick": "arrows", "right_stick": "cursor", "dpad": "arrows",
                   "cursor_speed": 900, "cross": "mouse_left", "circle": "mouse_right",
                   "square": "key:Space", "triangle": "key:Tab", "l1": "key:PageUp",
                   "r1": "key:PageDown", "l2": "mouse_middle", "r2": "key:LShift",
                   "start": "key:Escape", "select": "key:F10", "l3": "none", "r3": "none",
                   "ps": "action:settings"}
```

**Validation in `load()`:**
- `controls = cfg.setdefault("controls", {})`.
- `[touch] keypad` maps onto `default_layout` when `[controls] default_layout` is absent: `"auto"` → `"keys"`, `"hidden"` → `"hidden"`.
- Defaults: `default_layout` is `"keys"` when neither key is present. This keeps today's behaviour for games that haven't opted into the pad; the stub game sets `"pad"`. `pad` defaults to `"mapped"`.
- `pad` must be one of `native`, `mapped`, `off`.
- `[controls.mapped]` is merged over `MAPPED_DEFAULTS`. Unknown keys are rejected.
  - Button values must match `^(key:[A-Za-z0-9]+|mouse_(left|right|middle)|wheel_(up|down)|action:(settings|system_keyboard|edit_layout)|none)$`. `key:` names are checked against the same list as `scancode_from_name`; keep a Python copy in `game_config.py` with a comment that it mirrors `host/controls/layout.cpp`.
  - `dpad` must be `arrows`, `wasd` or `none`.
- `[controls.native]` accepts `xinput` and `dinput` (bool, default true), `axes` (a list of 6 axis names from `x y z rx ry rz`, default `["x","y","z","rz","rx","ry"]`) and `buttons` (a list of 13 pad button names, default `["square","cross","circle","triangle","l1","r1","l2","r2","select","start","l3","r3","ps"]`).
- `SETTINGS_ROWS`: rename the `"keypad"` entry to `"controls"`, and keep accepting `"keypad"` as an old spelling mapped to `"controls"`. The bit position stays the same, so `DISPLAY_KEYPAD_BIT` keeps its value; rename the constant to `DISPLAY_CONTROLS_BIT` in `mods/display_settings.h`.

**`gen_game_config.py`:** replace the `RECOMP_TOUCH_KEYPAD_HIDDEN` line with:

```python
    controls = cfg["controls"]
    lines.append("#define RECOMP_CONTROLS_DEFAULT_LAYOUT %s" % c_string(controls["default_layout"]))
    lines.append("#define RECOMP_CONTROLS_PAD %d" % ("off", "mapped", "native").index(controls["pad"]))
    lines.append("#define RECOMP_CONTROLS_MAPPED %s" % c_string(
        ";".join("%s=%s" % (k, controls["mapped"][k]) for k in sorted(controls["mapped"]))))
    native = controls["native"]
    lines.append("#define RECOMP_CONTROLS_XINPUT %d" % int(native["xinput"]))
    lines.append("#define RECOMP_CONTROLS_DINPUT %d" % int(native["dinput"]))
    lines.append("#define RECOMP_CONTROLS_NATIVE_AXES %s" % c_string(",".join(native["axes"])))
    lines.append("#define RECOMP_CONTROLS_NATIVE_BUTTONS %s" % c_string(",".join(native["buttons"])))
```

Grep for every `RECOMP_TOUCH_KEYPAD_HIDDEN` user and switch it over.

**`games/stub/game.toml`:** replace `[touch]` with:

```toml
[controls]
# The on-screen controls a fresh profile shows when no hardware keyboard is attached:
# "pad", "keys", "pad+keys" or "hidden". pad: "mapped" (keys and mouse), "native"
# (DirectInput joystick and XInput) or "off".
default_layout = "pad"
pad = "mapped"
```

- [ ] **Step 1: Write the failing Python tests** in `tests/test_game_config.py`. Replace `test_touch_keypad_knob` with `test_controls_section`, which checks:
  - The defaults with no section: `keys`, `mapped`, and `MAPPED_DEFAULTS`.
  - The old `[touch] keypad = "hidden"` → `hidden`.
  - A bad `pad` value, a bad button target, an unknown mapped key, and `native.axes` of the wrong length each raise `ValueError`.
  - The header contains `RECOMP_CONTROLS_PAD 1`, and `RECOMP_CONTROLS_MAPPED` contains `cross=mouse_left`.
  - The `rows = [..., "keypad"]` spelling still loads.

  Run `.venv/bin/python -m pytest tests/test_game_config.py -q`. Expected: FAIL.
- [ ] **Step 2:** Implement the Python side. Run it again. Expected: PASS.
- [ ] **Step 3: Write failing native tests.** Find the existing keypad settings tests (`grep -rn mods_keypad mods/tests`) and replace them with `mods_controls_*` equivalents:
  - **Defaults:** `set_names({"pad","keys","pad+keys"})`, then `init("keys")`, gives layout index 1.
  - **Migration:** seed a settings file with `{"host.keypad/left": 0, "host.keypad/right": 1, "host.keypad/size": 2}`, then `mods_settings_load`, then `init`. Expect layout = index of `keys`, `hidden` = 1 and `size` = 2.
  - **Hidden choice:** nudging `layout` past the last name reaches Hidden, and `mods_controls_layout_name()` returns `""`.
  - **Edit request:** nudging EDIT sets the request, and `take` clears it.
- [ ] **Step 4:** Implement `controls_settings.cpp`, modelled on `keypad_settings.cpp`: atomics for the values, `mods_settings_declare` with `MODS_OWNER_RUNTIME` and `"host.controls"`. Add `mods_settings_stored_value` to `settings.cpp` (a `stored()` lookup). Update `settings_page.cpp` and `options_menu.cpp` to list the `CONTROLS_*` rows under `DISPLAY_CONTROLS_BIT`. In `mods_page_init`, call `mods_controls_init(RECOMP_CONTROLS_DEFAULT_LAYOUT)`. The names come from `mods_controls_set_names`, which the host calls first, because `mods/` must not depend on `host/controls`.
- [ ] **Step 5:** Remove `keypad_settings.*` from `mods/CMakeLists.txt` and delete the files. `host/sdl/main.cpp` still uses `mods_keypad_*` until Task 7, so make this task's commit build by pointing main.cpp at a temporary adapter:
  - `v.left = !(mods_controls_hidden_groups() & 1)`, `v.right = !(… & 2)`, `v.size = mods_controls_value(CONTROLS_SIZE_ROW)`.
  - The tab toggle becomes `mods_controls_set_hidden_groups(bits ^ (1 << side))`.
  - Task 7 deletes this adapter.
- [ ] **Step 6:** Run `tools/test.py` and `tools/test.py --native`. Expected: both green.
- [ ] **Step 7:** Commit: `Controls: settings rows, keypad migration and [controls] in game.toml`

---

### Task 7: Overlay and host wiring; retire the keypad

**Files:**
- Create: `host/controls/overlay.h`, `host/controls/overlay.cpp`, `host/controls/raster.h`, `host/controls/raster.cpp`. `raster` starts with the flat fill and text moved out of `keypad_overlay.cpp`'s `Canvas`; Task 8 extends it.
- Create: `host/controls/controls_host.h`, `host/controls/controls_host.cpp`
- Delete: `host/keypad_overlay.h`, `host/keypad_overlay.cpp`
- Modify: `host/keypad_layout.h`/`.cpp`. Remove the tables, the geometry, `KeypadView` and `KeypadHit`; keep `KeypadScan`, `KeypadKey` and `KeypadRect` only if the oracle still needs them (the oracle can define its own copies instead, which is preferred), plus `keypad_is_modifier` and `keypad_modifier_bit`.
- Modify: `host/tests/keypad_tests.cpp` (keep the modifier and scancode tests; drop the geometry tests, which now live in `controls_tests` against the oracle)
- Modify: `host/present.h` (lines 242–247), `host/present_thread.cpp` (lines 8, 50–51, 198, 784–787, 1653–1660)
- Modify: `host/sdl/main.cpp` (lines 35–37, 819–882, 1034–1075, 1150–1160), `host/CMakeLists.txt` (line 140 area: the app sources)

**Interfaces:**
- Produces:
```cpp
// host/controls/overlay.h
namespace controls {
struct DrawControl {           // one control, resolved for drawing
    Kind kind; Rect rect; std::string label; bool pressed; bool lit;
    PadButton button; double knob_x, knob_y; double base_x, base_y; uint8_t hat; bool floating;
    bool group_visible;        // toggles use label_off when false
    std::string label_off;
};
struct ControlsView {
    bool wanted = false;       // draw anything at all
    uint64_t revision = 0;     // changes when anything below changes
    int dw = 0, dh = 0;
    double opacity = 1.0;
    std::vector<DrawControl> controls;
    Rect controls_area;        // portrait: fill with the backdrop colour (Task 17)
    bool editing = false;      // Task 20
    std::vector<Rect> guides;  // Task 20
    int selected = -1;         // Task 20
    std::vector<DrawControl> toolbar; // Task 20
};
class Overlay {
  public:
    ~Overlay();
    void draw(gpu::Device *device, gpu::CommandBuffer cb, gpu::Texture target, int w, int h,
              const ControlsView &view);
};
ControlsView make_view(const Layout &l, const Router &r, const Screen &s, double opacity);
}
// host/present.h (replaces the keypad pair)
#include "controls/overlay.h"
void host_present_set_controls(const controls::ControlsView &view);
controls::ControlsView host_present_controls(void);
// host/controls/controls_host.h: called only from host/sdl/main.cpp
namespace controls {
struct HostHooks {
    void (*key)(int scancode, bool down);
    void (*touch_actions)(const std::vector<TouchAction> &actions); // Task 10
    void (*system_keyboard)();
    void (*open_settings)();
};
void host_init(const HostHooks &hooks);      // after mods_page_init's registration hook, before first frame
void host_set_screen(const Screen &s);
void host_set_wanted(bool keyboard_absent, bool controller_present); // per pump
bool host_finger_down(int64_t id, double px, double py, uint64_t now);
bool host_finger_motion(int64_t id, double px, double py, uint64_t now);
bool host_finger_up(int64_t id, uint64_t now);
bool host_finger_cancel(int64_t id);
void host_release_all();
void host_pump(uint64_t now);                // after events: settings changes, binding tick, publish
}
```

**`controls_host.cpp` responsibilities in this task:**
- Owns the `LayoutStore`. The profile dir is `std::string(mods_overlay_profile_dir()) + "/controls"`. The game dir is the bundled layouts dir: `<game data root>/controls`; find the data-root accessor in `host/game_path.h`, and use `""` if the game has no such directory.
- Owns the active `Layout`, `Router` and a `ControlsSink` implementation that forwards to `hooks`.
- Calls `mods_controls_set_names(store.names())` before `mods_page_init`. Find where `main.cpp` or `boot.cpp` calls `mods_page_init`, and call `host_init` just before it.
- **Each pump:**
  - If `mods_controls_layout_name()` or the form factor changed, reload the layout, apply `hidden` bits to group visibility, apply `size` to `layout.scale` (`{32,36,40}[size] / 36.0`, multiplied by the file's own scale), and call `router.set_layout`.
  - Set `router.set_enabled(wanted)`, where `wanted` is `keyboard_absent || recomp_env("KEYPAD")` for layouts with any key control. (Task 12 extends this rule.)
  - Build the view with `make_view` and call `host_present_set_controls` only when the revision changed.
  - The revision is a hash of the router generation, the settings values, the layout name and the screen.
- `ControlsSink::group_visibility_changed` writes `mods_controls_set_hidden_groups`.
- `switch_layout("next")` sets the `layout` row to the next name, wrapping and skipping Hidden. A named target selects that index.
- `tap()` is a no-op until Task 13.

**`overlay.cpp`:** keep the one-texture-per-raster approach from `keypad_overlay.cpp`, but with one raster for the whole view, bounded to the union of control rects. Re-rasterize when `view.revision` or the drawable size changes. Blit with the same `hud` pipeline code as `KeypadOverlay::blit`.

In this task, draw only:
- **Keys and toggles**, exactly as the keypad did: the backdrop `6,9,15,150` behind grid groups, keys `40,48,64,200`, lit keys `120,160,255,220`, and the same text colours. Toggles use the tab colours and `label`/`label_off`.
- **Other kinds** as a flat circle placeholder (`40,48,64,200`). Task 11 replaces this.
- **Opacity** multiplies every alpha.

The grid backdrop needs group rects, so add `std::vector<Rect> backdrops` to `ControlsView`, filled from visible grid groups.

**`main.cpp`:**
- Delete the keypad block (819–882).
- `touch_release_all` calls `g_touch.cancel_all` and then `controls::host_release_all()`.
- The finger handlers:
  - `FINGER_CANCELED`: `if (controls::host_finger_cancel(id)) break;`
  - `FINGER_DOWN`: compute drawable coordinates (`f.x * dw`, `f.y * dh`); `if (controls::host_finger_down(...)) break;`
  - `FINGER_MOTION` and `FINGER_UP`: `if (controls::host_finger_motion/up(...)) break;`
- In `after_events`, replace the keypad block with `controls::host_set_screen(...)` (the safe area converted to drawable pixels via `SDL_GetWindowSafeArea` × scale), `controls::host_set_wanted(platform_ui_keypad_wanted(), false)` and `controls::host_pump(SDL_GetTicksNS())`.
- The hooks: `key` → `push_touch_key`, `system_keyboard` → toggle `SDL_StartTextInput` the way the four-finger path does, `open_settings` → `mods_page_open(nullptr)`.

**`present_thread.cpp`:** replace the keypad members and draw call with `controls::Overlay controls_overlay;` and `controls_overlay.draw(device, cb, drawable, w, h, host_present_controls())` when `wanted`.

- [ ] **Step 1: Write a failing test** for `make_view` in `controls_tests.cpp`:
  - It uses the `keys` built-in on a 2360×1640 screen at scale 2 with Shift latched.
  - It expects 77 key controls plus 2 toggles, with Shift `lit`.
  - It expects the two backdrops to equal the two group rects.
  - After the left group is hidden, the left keys are gone, the left toggle stays with `group_visible == false`, and there is one backdrop.

  Add `overlay_view.cpp` holding `make_view` alone (no GPU), so `controls_tests` can link it; `overlay.cpp` keeps the GPU part.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement everything above.
- [ ] **Step 4:** Run `tools/test.py --native`. Expected: all green, including `keypad_tests` (the modifier tests), `controls_tests` and `input_touch_tests`.
- [ ] **Step 5: Manual check on the Mac.** Build and launch the stub game with the keypad forced, and screenshot it:
  - Command: `RECOMP_KEYPAD=1 .venv/bin/python tools/build.py --target app` followed by the app launch documented in `docs/`; find the run command with `grep -rn "RECOMP_KEYPAD" docs README.md`.
  - The keypad must look identical to a screenshot taken from `main` with the same command, at the same size and position.
  - If the stub game can't run, say so in the report and rely on the geometry oracle.
- [ ] **Step 6:** Commit: `Controls: draw and route the keypad through the layout system`

---

## Phase 2: on-screen pad, mapped binding, DualSense look

### Task 8: Raster primitives

**Files:**
- Modify: `host/controls/raster.h`, `host/controls/raster.cpp`, `host/CMakeLists.txt` (add `raster.cpp` to `controls_tests`), `host/tests/controls_tests.cpp`

**Interfaces:**
- Produces:
```cpp
namespace controls {
struct Rgba { uint8_t r, g, b, a; };
struct Paint {                       // a fill: flat, or a gradient between two colours
    enum Kind { Flat, Linear, Radial } kind = Flat;
    Rgba c0{}, c1{};
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Linear: from (x0,y0) to (x1,y1); Radial: centre x0,y0, radius x1
};
class Canvas {                       // premultiplied RGBA, source-over, anti-aliased by coverage
  public:
    Canvas(std::vector<uint8_t> &px, int w, int h, double opacity = 1.0);
    void rect(double x, double y, double w, double h, const Paint &p);
    void round_rect(double x, double y, double w, double h, double radius, const Paint &p);
    void disc(double cx, double cy, double r, const Paint &p);
    void ring(double cx, double cy, double r_outer, double r_inner, const Paint &p);
    // A closed polygon (even-odd), e.g. a dpad arrow or a trapezoid.
    void polygon(const std::vector<std::pair<double, double>> &pts, const Paint &p);
    // A polyline stroke with round joins; used for the face glyphs.
    void stroke(const std::vector<std::pair<double, double>> &pts, double width, bool closed,
                const Paint &p);
    void text(int x, int y, const char *s, Rgba c, int scale = 2); // 6x8 font
    int text_width(const char *s, int scale = 2) const;
    Rgba at(int x, int y) const;
};
}
```

Coverage uses 4×4 supersampling per pixel, evaluated only inside the shape's bounding box. Signed-distance evaluation is fine for discs, rings and round rects. Polygons use the even-odd point test per sample. A stroke is the union of capsules (segment distance ≤ width/2).

- [ ] **Step 1: Write the failing tests** on a 64×64 canvas:
  - **Disc at (32,32), r=16, flat white opaque:**
    - The centre pixel is `255,255,255,255`.
    - (32,10) is transparent.
    - The edge pixel (48,32) has alpha strictly between 0 and 255.
    - The total alpha sum is within 2% of π·16²·255.
  - **Ring:** the centre is transparent, (32,20) with r_outer 14 and r_inner 10 is opaque.
  - **Radial gradient** from red to blue: the centre is red, and near the edge the colour is closer to blue than red.
  - **Opacity 0.5:** the disc centre alpha is 127 or 128, and the colour is premultiplied (r = alpha).
  - **Polygon triangle** (0,0), (63,0), (0,63): (5,5) is opaque, (60,60) is transparent.
  - **Stroke** of a horizontal line width 4 at y=32: (32,32) is opaque, (32,36) is transparent.
  - **Source-over:** a half-alpha black rect over an opaque white disc gives about 128 grey.
  - **Text:** "A" at scale 2 sets some pixels and `text_width("AB") == 24`.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement. `overlay.cpp` from Task 7 switches to `Canvas`; its flat rects become `rect(...)` with `Paint::Flat`.
- [ ] **Step 4:** Run `controls_tests` and `tools/test.py --native`. Expected: PASS.
- [ ] **Step 5:** Commit: `Controls: anti-aliased raster primitives`

---

### Task 9: Pad controls in the router, and the virtual pad

**Files:**
- Create: `host/controls/vpad.h`, `host/controls/vpad.cpp`
- Modify: `host/controls/router.{h,cpp}`, `host/CMakeLists.txt`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Produces:
```cpp
namespace controls {
enum PadBit : uint16_t { // bit = 1 << int(PadButton)
    kPadCross = 1 << 0, kPadCircle = 1 << 1, kPadSquare = 1 << 2, kPadTriangle = 1 << 3,
    kPadL1 = 1 << 4, kPadR1 = 1 << 5, kPadL2 = 1 << 6, kPadR2 = 1 << 7, kPadL3 = 1 << 8,
    kPadR3 = 1 << 9, kPadSelect = 1 << 10, kPadStart = 1 << 11, kPadPs = 1 << 12 };
enum PadHat : uint8_t { kHatUp = 1, kHatRight = 2, kHatDown = 4, kHatLeft = 8 };
struct PadState {
    uint16_t buttons = 0;
    uint8_t hat = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0; // -1..1, +y down (screen convention)
    float l2 = 0, r2 = 0;                  // 0..1
    bool operator==(const PadState &o) const;
    bool operator!=(const PadState &o) const { return !(*this == o); }
};
PadState merge(const PadState &a, const PadState &b); // OR buttons/hat; per axis larger |v|
// A stick's output for a finger offset (dx, dy) in pixels from its centre.
void stick_output(double dx, double dy, double radius_px, double deadzone, float *x, float *y);
// Dpad direction for an offset from its centre; half = half the dpad's size in pixels.
uint8_t dpad_hat(double dx, double dy, double half);
struct PadEdge { uint32_t sequence; uint8_t kind; uint8_t index; int32_t value; };
// kind 0: button index (value 0/1); 1: hat (value = PadHat bits); 2: axis index 0..5 (value * 32767)
enum { kPadSourceTouch = 0, kPadSourceController = 1, kPadSourceCount = 2 };
class Vpad { // thread-safe (one mutex); the host writes, the guest reads
  public:
    void set_source(int source, const PadState &s); // queues edges for what changed in the merge
    PadState state() const;
    uint32_t packet() const;                        // bumps on every merged change
    bool next_edge(uint32_t after_sequence, PadEdge *out) const; // oldest edge newer than after
    void request_rumble(uint16_t low, uint16_t high); uint64_t rumble_serial() const;
    void rumble(uint16_t *low, uint16_t *high) const;
    bool controller_connected() const; void set_controller_connected(bool on);
    void reset();
};
Vpad &vpad(); // the process-wide pad
// router additions
class Router { public: /* ... */ const PadState &pad() const; };
}
```

The edge queue is a ring of 256 entries. `next_edge` scans for the first entry with a sequence greater than `after_sequence`. A reader that falls more than 256 behind loses the oldest edges, which matches DirectInput buffer overflow; `DI_BUFFEROVERFLOW` is handled in Task 15.

**Math:**
- `stick_output`:
  - `m = hypot(dx, dy) / radius`, clamped to 1.
  - If `m <= deadzone`, the output is 0.
  - Otherwise `k = (m - deadzone) / (1 - deadzone)`, and the output is `(dx, dy) / hypot * k`.
- `dpad_hat`:
  - If `hypot < 0.25 * half`, the result is 0.
  - Otherwise the angle picks one of 8 sectors, 45° each and centred on the axes and diagonals. A diagonal sets two bits.

**Router behaviour:**
- **Button:** down sets its bit. For `l2`/`r2` it also sets that axis to 1. Up clears both.
- **Dpad:** down and motion set the hat from the offset to the rect centre. Up clears it.
- **Stick:**
  - Down: a floating stick sets its base to the finger position, clamped so the base circle stays inside the rect; a fixed stick uses the rect centre.
  - Down and motion: the output comes from `stick_output(finger - base, radius = rect.w/2, deadzone)`, and the stick's `knob_x`/`knob_y` state is updated.
  - Up: resets the output to 0 and the base to the default.
- A finger that lands on a floating stick's rect is claimed. The stick's rect is its whole zone.
- **The router's own pad state** is the OR of all owned pad controls. Two fingers on the same button hold it until both lift.

- [ ] **Step 1: Write the failing tests:**
  - **`stick_output` values:**
    - Offset (0,0) → 0.
    - (radius·0.1, 0) with deadzone 0.15 → 0.
    - (radius, 0) → (1, 0).
    - (2·radius, 0) → (1, 0).
    - (radius·0.575, 0) → x ≈ 0.5 (±0.01).
    - Diagonal (r, r) → magnitude 1.
  - **`dpad_hat` values:** (0, −h) → up; (h, h) → right|down; (0.1h, 0) → 0.
  - **`merge`:** buttons OR, and lx 0.2 against −0.7 gives −0.7.
  - **`Vpad` edges:**
    - `set_source(0, {cross})` → `packet` bumps, and `next_edge(0)` gives kind 0, index 0, value 1.
    - Setting it again with the same state adds no edge.
    - A controller source holding cross while the touch source releases it → the merged state still has cross, and no edge is added.
    - 300 changes → `next_edge(0)` returns an entry with sequence > 44.
  - **Router, using a layout with a `cross` button, an `l2` button, a dpad and a floating left stick:**
    - Cross held → `pad().buttons == kPadCross`.
    - `l2` held → `l2 == 1`.
    - A finger at the stick's right edge region: base at the finger, then motion by +radius → lx == 1, and `state().knob_x == 1`.
    - Lifting clears everything.
    - Dpad motion from up to right changes the hat without a lift.
    - `cancel_all` clears everything.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement. Add `vpad.cpp` to `controls_tests` and to the app sources. In `controls_host::host_pump`, call `vpad().set_source(kPadSourceTouch, router.pad())`.
- [ ] **Step 4:** Run the tests. Expected: PASS.
- [ ] **Step 5:** Commit: `Controls: pad buttons, dpad and sticks feed a shared virtual pad`

---

### Task 10: Mapped binding

**Files:**
- Create: `host/controls/binding.h`, `host/controls/binding.cpp`
- Modify: `host/input_touch.h` (add `Wheel` to `TouchAction::Kind`, with `int wheel = 0;` in notches, + up), `host/sdl/main.cpp` (`push_touch_action_now` handles `Wheel` by pushing `SDL_EVENT_MOUSE_WHEEL` with `y = wheel`, `mouse_x/mouse_y = a.x/a.y`, `windowID = ours`), `host/controls/controls_host.cpp`, `host/CMakeLists.txt` (add `binding.cpp` and `${HOST}/input_touch.cpp` if needed), `host/tests/controls_tests.cpp`

**Interfaces:**
- Consumes: `PadState`, `PadButton`, `pad_button_from_name`, `scancode_from_name`, `TouchAction`, `kTouchPanStep`, `kTouchTapTravel`.
- Produces:
```cpp
namespace controls {
enum class StickMode { Cursor, Arrows, Wasd, Scroll, Wheel, None };
struct Target {
    enum Type { None, Key, Mouse, Wheel, Action } type = None;
    int value = 0;       // Key: scancode; Mouse: 0 left 1 right 2 middle; Wheel: +1 up / -1 down
    std::string action;  // Action
};
struct MappedTable {
    StickMode left = StickMode::Arrows, right = StickMode::Cursor;
    StickMode dpad = StickMode::Arrows; // Arrows | Wasd | None
    double cursor_speed = 900;          // points per second at full deflection
    Target buttons[int(PadButton::Count)];
};
// "k=v;k=v" (RECOMP_CONTROLS_MAPPED), applied over *table. Unknown keys/values -> false + *error.
bool parse_mapped(const std::string &text, MappedTable *table, std::string *error);
std::string write_mapped(const MappedTable &table); // same syntax, every key, sorted
std::string target_name(const Target &t);           // "key:Space", "mouse_left", ...
class Binding {
  public:
    void set_table(const MappedTable &t);
    void set_bounds(double w, double h);         // window points
    void set_cursor(double x, double y);         // a real pointer or touch moved it
    // One input tick: emits TouchActions (window points) and host actions.
    void tick(const PadState &pad, uint64_t now_ns, std::vector<TouchAction> *out,
              std::vector<std::string> *actions);
    void release_all(std::vector<TouchAction> *out);
    double cursor_x() const; double cursor_y() const;
};
}
```

**Evaluation per tick** (`dt` = now − last, clamped to 50 ms; the first tick has `dt = 0`):
- **Buttons:** on a rising or falling edge of a bit, apply the button's target.
  - `Key`: a key down/up action.
  - `Mouse`: a `Motion` to the cursor with `place = true`, then a `Button` down/up at the cursor.
  - `Wheel`: one `Wheel` action with `wheel = value` on the rising edge only.
  - `Action`: the name goes into `actions` on the rising edge.
- **Stick modes** (`v` = the stick's x, y):
  - `Cursor`: `speed = cursor_speed * |v|²` along `v`. Move the cursor by `speed * dt`, clamp it to the bounds, and emit `Motion` (`place = true`) only when it moved by at least 0.5 pt.
  - `Arrows`/`Wasd`: per axis, press when the value passes 0.5 in a direction and release when it falls below 0.35. Keys: x− Left/A, x+ Right/D, y− Up/W, y+ Down/S.
  - `Scroll`: accumulate `v * dt * 20 * kTouchPanStep` per axis. Each whole `kTouchPanStep` taps (down and up) the arrow key for that direction, the same as `TouchMapper`'s pan.
  - `Wheel`: accumulate `-v.y * dt * 8` notches, and emit `Wheel` for each whole notch.
- **Dpad:** `Arrows`/`Wasd` press the direction keys while the hat bits are held.
- **`release_all`:** releases every key and mouse button the binding holds and resets the accumulators.

**`controls_host`:**
- Owns a `Binding` configured from `RECOMP_CONTROLS_MAPPED`, followed by `<profile>/controls/binding.txt` if it exists. The file uses the same `k=v;` syntax; this replaces the spec's `binding.json` for simplicity (update the spec, Task 22).
- In `host_pump`, when `RECOMP_CONTROLS_PAD == 1`: `binding.tick(vpad().state(), now, &actions, &names)`, then `hooks.touch_actions(actions)`, and each name is routed like `sink.action`.
- Add `void host_pointer_moved(double x, double y)`, which `main.cpp` calls from `handle_mouse_move` (real events only; skip `which == SDL_TOUCH_MOUSEID` for events the binding pushed itself) and from the touch placement. It calls `binding.set_cursor`.
- `host_release_all` also calls `binding.release_all`.

- [ ] **Step 1: Write the failing tests:**
  - **Parsing:**
    - `parse_mapped("cross=key:Space;left_stick=cursor")` changes only those two entries.
    - `"cross=key:Nope"` fails, and so does `"bogus=none"`.
    - `write_mapped` → `parse_mapped` round-trips.
  - **Cross mapped to `mouse_left`, cursor at (100, 100):**
    - Cross down → `Motion(100,100)`, `Button(0, down)`.
    - Cross up → `Button(0, up)`.
  - **Right stick on Cursor, speed 900, bounds 1000×800:**
    - rx = 1 for three ticks 10 ms apart moves the cursor by 18 ± 0.5.
    - rx = 0.5 moves it by a quarter of that.
    - The cursor clamps at the right edge.
  - **Left stick on Arrows:**
    - lx 0.6 → Right down.
    - 0.4 → nothing.
    - 0.3 → Right up.
    - ly −0.9 → Up down.
  - **Wheel:** `l1 = wheel_up` → one `Wheel +1` per press.
  - **Action:** `ps = action:settings` → `actions == {"settings"}` once per press.
  - **Scroll:** lx = 1 for 1 s in 10 ms ticks → about 20 Right taps (±2).
  - **`release_all`:** with Right and mouse-left held, emits both releases.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the tests and `tools/test.py --native`. Expected: PASS.
- [ ] **Step 5:** Commit: `Controls: map the pad onto keys and mouse per game`

---

### Task 11: DualSense look, pad layouts, layout cycling, opacity

**Files:**
- Modify: `host/controls/overlay.cpp`, `host/controls/overlay_view.cpp`, `host/controls/builtin_layouts.cpp`, `host/controls/controls_host.cpp`, `host/tests/controls_tests.cpp`
- Create: `host/controls/pad_art.h`, `host/controls/pad_art.cpp`. These are pure functions that paint one control into a `Canvas`; they're testable without a GPU.

**Interfaces:**
- Produces:
```cpp
namespace controls {
// Paints c at its rect, relative to the canvas origin (ox, oy). No knob for sticks (drawn separately).
void paint_control(Canvas &cv, const DrawControl &c, int ox, int oy);
void paint_knob(Canvas &cv, double cx, double cy, double r, bool pressed); // into its own small canvas
}
```

**Art:**
- **Face buttons:**
  - A disc with a radial fill from `58,62,72` to `22,24,30`, a 2 px rim ring `90,96,110`, and a stroked glyph with width `r*0.14`.
  - Glyph colours: ✕ `124,178,232`, ○ `255,102,102`, □ `255,105,248`, △ `64,226,160`.
  - While pressed, the fill brightens by 40 and the radius is ×0.94.
- **Glyph shapes** (relative to the centre, s = r·0.42):
  - ✕: two diagonals.
  - ○: a closed 24-point circle of radius s.
  - □: a closed square with side 1.6·s.
  - △: a closed triangle `(0,−s), (0.95s, 0.7s), (−0.95s, 0.7s)`.
- **Shoulders:** a polygon trapezoid with a linear fill, and the label centred. `l2`/`r2` fill from the bottom in proportion to the trigger value (use `pressed ? 1 : 0` for the on-screen control).
- **Dpad:** a recessed disc `18,20,26` with four arrow polygons that light up individually by `hat`.
- **Stick:** a base ring (outer r, inner r·0.86) over a darker disc. Floating sticks draw the base at 35% alpha while idle, at `base_x/base_y` while held.
- **Knob:** a disc r·0.45 with a radial highlight offset up and left (a concave look), in its own 128×128 texture. It's uploaded once per size, and its position is a second hud blit per held stick. `Overlay` keeps one knob texture.
- **Select, start and PS:** small round rects; PS is a disc labelled "PS".
- **Keys and toggles:** round rects with radius 6 px × scale, a subtle linear fill (`46,54,72` → `34,40,56`) and lit keys `120,160,255`.

**Built-in layouts (tablet)** (points, inside the safe area):
- **`pad`:**
  - Left stick: floating, `bottom-left`, x 24, y 24, radius 110.
  - Dpad: `bottom-left`, x 250, y 40, size 130.
  - Face buttons (size 64) in a diamond anchored `bottom-right`: △ (x 92, y 164), ○ (x 24, y 96), ✕ (x 92, y 28), □ (x 160, y 96).
  - Right stick: floating, `bottom-right`, x 250, y 24, radius 90.
  - L1 (`top-left`, x 24, y 24, w 110, h 44) and L2 (x 24, y 76). R1 and R2 mirrored on the `top-right`.
  - Select (`bottom-center`, x −70, y 16, w 70, h 30), PS (`bottom-center`, x 0, y 12, size 40), Start (`bottom-center`, x 70, y 16, w 70, h 30).
  - L3 and R3 are not on screen by default.
  - A toggle `{"target":"next","label":"KEYS"}` at `top-center`, x 0, y 8, w 72, h 28.
- **`keys`:** add the same cycle toggle (`label "PAD"`) at `top-center` to the Task 3 layout. The regression test ignores group 2 control 2.
- **`pad+keys`:**
  - The `keys` groups with the grid key at 30.
  - `pad` group `sticks` with only the two sticks (radius 80), anchored `center-left` and `center-right`, and the face diamond scaled to 52 above the right keyboard half: anchor `bottom-right`, with y offsets increased by the keyboard height (the grid height of 5·34 = 170, plus 12).
  - The cycle toggle labelled "NEXT".

**`controls_host`:**
- Applies `mods_controls_value(CONTROLS_OPACITY_ROW) / 100.0` multiplied by the layout's own `opacity`.
- Layout cycling is already done through `switch_layout` in Task 7.

**LayoutStore test update** (from Task 4): `load("pad", Tablet)` with a broken game `pad.json` now falls through to the built-in `pad` and still reports the problem.

- [ ] **Step 1: Write the failing tests:**
  - `paint_control` for a `cross` button at 128×128: the centre pixel is non-transparent, and there is a pixel on the ✕ diagonal whose blue channel exceeds its red channel.
  - Pressed brightens the fill at a point off the glyph.
  - Every built-in `(name, Tablet)` parses.
  - Every control of every built-in lies inside a 1180×820 pt screen (at scale 1, with no `stack_on` group hidden) and no two pad controls overlap. Keys may touch their own grid only.
  - The LayoutStore fallback test described above.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `tools/test.py --native`, then run the app on the Mac with `RECOMP_KEYPAD=1` and the stub game (or any game repo at hand; see memory notes). Screenshot `pad`, `keys` and `pad+keys` and look at them. Report the screenshot paths.
- [ ] **Step 5:** Commit: `Controls: DualSense-styled pad and the pad, keys and pad+keys layouts`

---

## Phase 3: physical controllers, auto-hide, rumble, haptics

### Task 12: SDL_Gamepad and auto-hide

**Files:**
- Create: `host/controls/gamepad_sdl.h`, `host/controls/gamepad_sdl.cpp`
- Modify: `host/sdl/main.cpp` (add `SDL_INIT_GAMEPAD` to the `SDL_Init` flags; route `SDL_EVENT_GAMEPAD_ADDED`, `_REMOVED`, `_BUTTON_*` and `_AXIS_MOTION` to `gamepad_sdl`), `host/controls/router.{h,cpp}`, `host/controls/controls_host.cpp`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Produces:
```cpp
namespace controls {
// SDL-free: SDL gamepad button/axis ids (as ints) -> PadState. Spelled out like KeypadScan.
PadState pad_from_sdl(const bool buttons[21], const int16_t axes[6]);
enum class LayoutContent { Keys, Pad, Mixed };
LayoutContent layout_content(const Layout &l);
// Whether the layout shows, given the devices present and the setting.
bool layout_wanted(LayoutContent c, bool keyboard_present, bool controller_present,
                   bool pad_with_controller, bool forced);
// gamepad_sdl.h (SDL)
void gamepad_handle_event(const SDL_Event &e); // opens the first pad, closes on removal
void gamepad_poll();                           // per pump: state -> vpad source 1; rumble serial -> SDL
bool gamepad_connected();
bool gamepad_rumble(uint16_t low, uint16_t high, uint32_t ms); // false if no pad
}
```

**The SDL3 mapping** in `pad_from_sdl`: the button indexes follow `SDL_GamepadButton`.

| SDL button | Pad |
|---|---|
| SOUTH 0 | cross |
| EAST 1 | circle |
| WEST 2 | square |
| NORTH 3 | triangle |
| BACK 4 | select |
| GUIDE 5 | ps |
| START 6 | start |
| LEFT_STICK 7 | l3 |
| RIGHT_STICK 8 | r3 |
| LEFT_SHOULDER 9 | l1 |
| RIGHT_SHOULDER 10 | r1 |
| DPAD_UP 11, DOWN 12, LEFT 13, RIGHT 14 | hat |

- Axes follow `SDL_GamepadAxis`: LEFTX 0, LEFTY 1, RIGHTX 2, RIGHTY 3, LEFT_TRIGGER 4, RIGHT_TRIGGER 5.
- Sticks are `v / 32767` clamped to ±1, with a 0.08 radial dead zone.
- Triggers are `v / 32767` clamped to 0..1. A trigger above 0.5 also sets the l2/r2 bit.
- The test file asserts these ints against the SDL enums with a `static_assert`, the way `keypad_tests` does.

**`layout_wanted`:**
- `forced` returns true.
- `Keys` returns `!keyboard_present`.
- `Pad` returns `!controller_present || pad_with_controller`.
- `Mixed` returns `!keyboard_present || !controller_present || pad_with_controller`.

**When the active layout isn't wanted,** `controls_host` still draws and routes that layout's toggle controls, so the player can switch. `Router` gains `set_toggles_only(bool)`: hit tests only consider toggles, and `make_view` only emits toggles.

**`platform_ui_keypad_wanted()`** keeps its meaning (touch device with no hardware keyboard). `host_set_wanted(keyboard_absent, gamepad_connected())` is passed on to `layout_wanted`. `vpad().set_controller_connected` mirrors the connection state.

**Desktop:** the overlay is off unless `RECOMP_KEYPAD` is set. `platform_ui_keypad_wanted()` already returns false there, so `layout_wanted(Pad, keyboard_present = true, …)` needs care: pass `forced = recomp_env("KEYPAD") != nullptr`, and on desktop, `controls_host` hides everything unless forced. Add `bool platform_ui_touch_device()` to `platform_ui.h`: iOS true; desktop file true only when `SDL_GetPlatform()` is "Android".

- [ ] **Step 1: Write the failing tests:**
  - `pad_from_sdl`: South pressed → cross; LEFTX 32767 → lx 1; LEFT_TRIGGER 20000 → l2 ≈ 0.61 with the l2 bit set; DPAD_LEFT → `kHatLeft`.
  - The `layout_wanted` truth table.
  - `layout_content` of the three built-ins is Keys, Pad and Mixed.
  - Router `set_toggles_only`: a key is not hit, the cycle toggle is.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `tools/test.py --native`. **Manual check:** connect any controller to the Mac, run with `RECOMP_KEYPAD=1 RECOMP_CONTROLS_TRACE=1`, and confirm the pad overlay hides and pad presses reach the game. Add `RECOMP_CONTROLS_TRACE` logging to `controls_host` that prints the merged pad state on change. If no controller is available, say so.
- [ ] **Step 5:** Commit: `Controls: physical controllers feed the pad and hide the on-screen one`

---

### Task 13: Rumble and button haptics

**Files:**
- Create: `host/controls/haptics.h`
- Modify: `host/sdl/platform_ui.h`, `host/sdl/platform_ui_desktop.cpp`, `host/sdl/platform_ui_ios.mm`, `platform/android/app/src/main/java/dev/recompkit/RecompActivity.java`, `host/controls/controls_host.cpp`, `host/controls/gamepad_sdl.cpp`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Produces:
```cpp
// host/controls/haptics.h
namespace controls {
// The rumble decision: controller if connected, else the device motor on a touch device, else nothing.
enum class RumbleSink { None, Controller, Device };
RumbleSink rumble_sink(bool controller_connected, bool touch_device);
}
// host/sdl/platform_ui.h
void platform_ui_haptic_tap();                                  // light impact; desktop no-op
void platform_ui_device_rumble(uint16_t low, uint16_t high);    // 0,0 stops; desktop no-op
```

**iOS (`platform_ui_ios.mm`):**
- `platform_ui_haptic_tap`: a cached `UIImpactFeedbackGenerator` with `UIImpactFeedbackStyleLight`. Call `prepare` once and `impactOccurred` on the main thread (`dispatch_async(dispatch_get_main_queue(), …)`).
- `platform_ui_device_rumble`: a cached `CHHapticEngine`, started lazily, returning early if `CHHapticEngine.capabilitiesForHardware.supportsHaptics` is false.
  - On a non-zero value, start a continuous `CHHapticEvent` (duration 30 s) with intensity `max(low, high) / 65535.0` and sharpness 0.3, through a `CHHapticAdvancedPatternPlayer`. Later changes call `sendParameters` with `CHHapticDynamicParameterIDHapticIntensityControl`.
  - Zero stops the player.
  - Link the `CoreHaptics` framework in `host/CMakeLists.txt` for iOS; find where UIKit is linked.

**Android:**
- `RecompActivity.java` gains two static methods:
  - `hapticTap()`: `getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)` on the UI thread.
  - `deviceRumble(int low, int high)`: a `Vibrator` from `getSystemService(VIBRATOR_SERVICE)` (or `VibratorManager` on API 31+). Non-zero calls `vibrate(VibrationEffect.createOneShot(60000, amplitude))` with `amplitude = max(1, max(low, high) * 255 / 65535)` when `hasAmplitudeControl()`, else `createOneShot(60000, DEFAULT_AMPLITUDE)`. Zero calls `cancel()`. Re-issue only when the amplitude bucket (÷16) changes.
- `platform_ui_desktop.cpp` calls these through JNI when `SDL_GetPlatform()` is `"Android"`: `SDL_GetAndroidJNIEnv()`, `FindClass("dev/recompkit/RecompActivity")`, `GetStaticMethodID`. Follow any existing JNI usage in the repo (`grep -rn "JNIEnv\|SDL_GetAndroidJNIEnv" host platform`). If none exists, guard the JNI code with `#if defined(__ANDROID__)` **inside `platform_ui_desktop.cpp` only**, which already carries the Android variant, and note the exception in the commit message.
- Add `<uses-permission android:name="android.permission.VIBRATE"/>` to `AndroidManifest.xml.in`.

**Desktop:** both functions are no-ops.

**`controls_host`:**
- `ControlsSink::tap()` calls `platform_ui_haptic_tap()` when `mods_controls_value(CONTROLS_HAPTICS_ROW)` is set.
- In `host_pump`, when `vpad().rumble_serial()` changed:
  - Read `rumble(&low, &high)` and route by `rumble_sink`:
    - Controller: `gamepad_rumble(low, high, 1000)`.
    - Device: `platform_ui_device_rumble`.
  - While non-zero, refresh the controller rumble every 500 ms, since SDL rumble has a duration. The device rumble lasts 60 s and is re-issued every 30 s.
  - On a disconnect with rumble active, stop the old sink and switch to the new one.

- [ ] **Step 1: Write the failing test:** the `rumble_sink` truth table.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `tools/test.py --native`. If the toolchains are present locally, build `ios-stub` and `android-stub` (`.venv/bin/python tools/build.py --preset ios-stub --stub` and `--preset android-stub --stub`; check `tools/build.py --help` and CI `.github/workflows/checks.yml` for the exact commands). Report which ran.
- [ ] **Step 5:** Commit: `Controls: rumble to the controller or the device, and button haptics`

---

## Phase 4: native input

### Task 14: Host pad callbacks for the guest side

**Files:**
- Modify: `dx/host_api.h`, `dx/host_api.cpp`, `host/controls/vpad.cpp`, `dx/tests/host_api_header_test.c` (if it enumerates declarations)

**Interfaces:**
- Produces (in the C section of `host_api.h`):
```c
typedef struct HostPadState {
    uint16_t buttons;   /* controls::PadBit */
    uint8_t hat;        /* controls::PadHat */
    uint8_t reserved;
    int16_t lx, ly, rx, ry; /* -32767..32767, +y down */
    uint8_t l2, r2;     /* 0..255 */
} HostPadState;
typedef struct HostPadEvent { uint32_t sequence; uint8_t kind; uint8_t index; int32_t value; } HostPadEvent;
/* 0 off, 1 mapped, 2 native (RECOMP_CONTROLS_PAD). Weak default: 0. */
int host_pad_mode(void);
/* Bit 0: serve DirectInput joystick; bit 1: serve XInput. Weak default: 0. */
int host_pad_native_apis(void);
/* The merged pad. Returns a packet number that changes with the state. Weak default: 0, zeroed. */
uint32_t host_pad_state(HostPadState *out);
/* The oldest edge newer than `after`. Weak default: 0 (none). */
int host_pad_next_event(uint32_t after, HostPadEvent *out);
/* Rumble request, 0..65535 each. Weak default: no-op. */
void host_pad_rumble(uint16_t low, uint16_t high);
/* Axis and button order for the DirectInput device (RECOMP_CONTROLS_NATIVE_*). Weak defaults: the spec order. */
const char *host_pad_native_axes(void);
const char *host_pad_native_buttons(void);
```

Strong definitions go in `vpad.cpp`. They use `RECOMP_CONTROLS_PAD`, `RECOMP_CONTROLS_DINPUT`, `RECOMP_CONTROLS_XINPUT` and `RECOMP_CONTROLS_NATIVE_*` from `game_config.h`; `vpad.cpp` must include it, so check how `host/input_gate.cpp` includes it. `vpad.cpp` then can't be linked into `controls_tests` without the header, so split the strong definitions into `host/controls/vpad_host_api.cpp` (app only).

- [ ] **Step 1:** Add a test to `controls_tests` for a helper `HostPadState to_host(const PadState&)`, declared in `vpad.h`:
  - lx −1 → −32767.
  - ly 0.5 → 16384 (rounded).
  - l2 1 → 255.
- [ ] **Step 2:** Run and confirm failure. Implement. Run `tools/test.py --native`. Expected: PASS, including the `dx_tests` header ABI check.
- [ ] **Step 3:** Commit: `Controls: pad state reaches the DirectX shims through host_api`

---

### Task 15: DirectInput joystick device

**Files:**
- Create: `dx/dinput_joystick.h`, `dx/dinput_joystick.cpp`, `dx/tests/pad_tests.cpp`
- Modify: `dx/dinput.cpp` (`DI_CreateDevice`, `DI_EnumDevices`, `DI_GetDeviceStatus`, `Device_GetCapabilities`, `Device_EnumObjects`, `Device_GetProperty`, `Device_SetProperty`, `Device_GetDeviceState`, `Device_GetDeviceData`, `Device_SetDataFormat`, `Device_GetObjectInfo`, `Device_GetDeviceInfo`, `Device_Poll`, `poll_device`), `dx/com.h` (if `DIDEVTYPE_JOYSTICK` or similar constants live there), `dx/CMakeLists.txt`

**Interfaces:**
- Consumes: the `host_pad_*` functions (Task 14).
- Produces:
```cpp
// dx/dinput_joystick.h: guest-memory helpers for the joystick device kind.
extern const uint8_t GUID_Joystick_[16];        // {6F1D2B70-D5A0-11CF-BFC7-444553540000}
extern const uint8_t GUID_RecompPadInstance_[16]; // fixed, kit-owned
extern const uint8_t GUID_RecompPadProduct_[16];  // fixed, kit-owned
bool joy_served();                                  // host_pad_mode()==2 && (host_pad_native_apis() & 1)
bool joy_guid(const uint8_t *guid16);               // GUID_Joystick_ or the instance GUID
bool joy_enum_matches(uint32_t devtype_filter, uint32_t di_version); // DI5/7 types and DI8 classes
uint32_t joy_devtype(uint32_t di_version);          // DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8) for <0x800, DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) for >=0x800
struct JoyObject { uint8_t guid[16]; uint32_t ofs; uint32_t type; const char *name; };
// DIJOYSTATE (80 bytes) or DIJOYSTATE2 (272 bytes) objects in the configured order.
const std::vector<JoyObject> &joy_objects();
struct JoyAxisRange { int32_t min = -32768, max = 32767; uint32_t deadzone = 0, saturation = 10000; };
// Writes the state in the set format (size = 80 or 272) at guest address `at`.
void joy_write_state(uint32_t at, uint32_t size, const HostPadState &s, const JoyAxisRange ranges[6]);
// The DIDEVICEOBJECTDATA offset/value for a pad event, given the ranges; false if not reported.
bool joy_event(const HostPadEvent &e, const JoyAxisRange ranges[6], uint32_t *ofs, uint32_t *data);
```

**Rules:**
- **Serving.** `joy_served()` gates everything. When it's false, all behaviour stays as today.
- **`DI_EnumDevices`:**
  - The filter matches the DI5/7 devtype `DIDEVTYPE_JOYSTICK = 4`, or `0` (all).
  - DI8 callers are recognised by `di_version >= 0x800` and use classes: `DI8DEVCLASS_ALL = 0`, `DI8DEVCLASS_GAMECTRL = 4`, `DI8DEVTYPE_GAMEPAD = 0x15`.
  - Add the pad entry with the instance GUID, the product GUID and the name "Recomp Virtual Pad".
  - `DIEDFL_ATTACHEDONLY` (flags bit 1) is satisfied, since the pad is always attached.
  - This file only has DI ≤ 7 today, while the `nfsmw` branch adds DI8. Keep the DI8 logic inside `joy_enum_matches` so their merge only has to call it.
- **`DI_CreateDevice`:** for `joy_guid`, create `K_DIDEVICE` with `dev_type = DIDEVTYPE_JOYSTICK`. Add a `joy_ranges[6]` array and `last_sequence` to `ComObj` in `dx/com.h`; check its field style.
- **`SetDataFormat`:**
  - Accepts sizes 80 (`DIJOYSTATE`) and 272 (`DIJOYSTATE2`).
  - Any other size is accepted when every object in the guest's `DIDATAFORMAT.rgodf` maps to a known axis, POV or button GUID. Write only the fields at their offsets, and record the mapping on the device.
  - Otherwise return `DIERR_INVALIDPARAM`.
  - Read the layout of `DIDATAFORMAT`/`DIOBJECTDATAFORMAT` from the DirectX headers (sizes 24 and 16).
- **`GetCapabilities`:** `dwAxes = 6`, `dwButtons = 13`, `dwPOVs = 1`, `dwFlags = DIDC_ATTACHED (1)`, with no force-feedback flag.
- **`EnumObjects`:**
  - Calls back once per object with `DIDEVICEOBJECTINSTANCEA`: 316 bytes in the DirectX 5 layout, 1100-something in the W layout. Mirror `DI_EnumDevices`' A/W handling and check the sizes against the headers.
  - Types use `DIDFT_AXIS`/`DIDFT_ABSAXIS` = 2, `DIDFT_BUTTON`/`DIDFT_PSHBUTTON` = 4, `DIDFT_POV` = 0x10, with the instance in bits 8–23.
  - It honours the `dwFlags` filter.
- **`GetObjectInfo`:** the same record for `DIPH_BYOFFSET`/`DIPH_BYID`.
- **`Get/SetProperty`:**
  - `DIPROP_RANGE` (4), `DIPROP_DEADZONE` (5) and `DIPROP_SATURATION` (6) apply per object (`DIPH_BYOFFSET`) or to all objects (`DIPH_DEVICE`).
  - `DIPROP_BUFFERSIZE` behaves as it does today.
  - The dead zone and saturation are applied in `joy_write_state` as DirectInput does: values inside the dead zone read as centre, values beyond the saturation read as the extreme, with linear scaling between.
- **State:**
  - POV: `0xFFFFFFFF` when centred, otherwise hundredths of degrees clockwise from north (up 0, up-right 4500, …).
  - Buttons: `0x80` when pressed.
  - Axes: the configured order (`host_pad_native_axes()`, e.g. `"x,y,z,rz,rx,ry"`) assigns the pad's lx, ly, rx, ry, l2, r2 to DIJOYSTATE fields `lX`(0), `lY`(4), `lZ`(8), `lRx`(12), `lRy`(16), `lRz`(20).
  - Triggers map 0..255 onto the full axis range, from min at 0 to max at 255.
  - POV is at offset 32. Buttons start at offset 48, in the order from `host_pad_native_buttons()`.
- **`GetDeviceData`:**
  - Drains `host_pad_next_event` after `last_sequence`, up to the caller's count.
  - Each event becomes a 16-byte `DIDEVICEOBJECTDATA` (DirectX 5 layout: ofs, data, timestamp, sequence) or a 20-byte one for DI8 (+appdata). Use the existing mouse path's size logic.
  - `DIGDD_PEEK` leaves `last_sequence` unchanged.
  - When the host's oldest retained sequence is newer than `last_sequence + 1`, return `DI_BUFFEROVERFLOW` (1).
- **`Poll`:** returns `DI_OK` for a joystick. `DI_NOEFFECT` (1) is also acceptable, but use `DI_OK`.
- **`GetDeviceStatus`:** `DI_OK` for the pad GUIDs when served.
- **`Acquire`/`Unacquire`:** same as the mouse. `GetDeviceState` without `Acquire` returns `DIERR_NOTACQUIRED`, as it does for the mouse today.

**`pad_tests.cpp`** is a new CTest `pad_tests` (label `nogame`), linked like `dx_tests`. It provides **strong** `host_pad_*` definitions backed by a test-controlled `HostPadState` and event list. Drive the COM objects the way `dx/tests/dx_tests.cpp` drives the mouse; read its DirectInput tests first and reuse its helpers, copying them into a small shared header if needed.

- [ ] **Step 1: Write the failing tests:**
  - With mode 2: `EnumDevices(DIDEVTYPE_JOYSTICK)` calls back once with the pad's name. With mode 1 it doesn't call back.
  - `CreateDevice(GUID_Joystick)` → `DI_OK`, then `SetDataFormat(80)`, `Acquire` and `GetDeviceState(80)`:
    - With lx = 32767, `lX == 32767`.
    - With a range of 0..1000 set, lx = 0 → `lX == 500`.
    - With a dead zone of 2000, lx = 3000 → centre (500).
    - Button cross → `rgbButtons[1] == 0x80`, per the default order square, cross.
    - Hat right|down → POV 13500.
    - l2 = 255 with the default axes → `lRx == max`.
  - `SetDataFormat(272)` works.
  - `GetDeviceData`: after events cross down and cross up → two records with `dwOfs == 48 + 1` and data `0x80`, then `0`. A second call returns none. `DIGDD_PEEK` doesn't consume.
  - `EnumObjects(DIDFT_ALL)` → 6 + 13 + 1 = 20 callbacks.
  - Custom format: a 2-axis, 2-button format places the fields at the given offsets.
- [ ] **Step 2:** Run `.venv/bin/cmake --build build/cmake/macos --target pad_tests && .venv/bin/ctest --test-dir build/cmake/macos -R pad_tests --output-on-failure`. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `pad_tests`, `dx_tests` and then `tools/test.py --native`. Expected: PASS.
- [ ] **Step 5:** Commit: `DirectInput: serve the virtual pad as a joystick when a game asks for native input`

---

### Task 16: XInput

**Files:**
- Create: `dx/xinput.cpp`
- Modify: `dx/CMakeLists.txt`, `dx/dx.cpp` (or wherever `dinput_register()` is called; register `xinput_register()` beside it), `dx/tests/pad_tests.cpp`

**Interfaces:**
- Produces: `void xinput_register();` and the import shims below, for each of `XINPUT1_3.dll`, `XINPUT1_4.dll` and `XINPUT9_1_0.dll`. Match the DLL name spelling `imports_register` expects: check how `"DINPUT.dll"` is spelled and whether the lookup is case-insensitive in `runtime/imports.cpp`.

| Export | Args | Behaviour |
|---|---|---|
| `XInputGetState(user, *state)` | 2 | See below |
| `XInputSetState(user, *vibration)` | 2 | User 0 served: `host_pad_rumble(left, right)` (two WORDs), then `ERROR_SUCCESS`; otherwise `ERROR_DEVICE_NOT_CONNECTED` (1167) |
| `XInputGetCapabilities(user, flags, *caps)` | 3 | `XINPUT_CAPABILITIES` (20 bytes): Type 1 (`XINPUT_DEVTYPE_GAMEPAD`), SubType 1 (`XINPUT_DEVSUBTYPE_GAMEPAD`), Flags 0, Gamepad with all button bits `0xFFFF` except 0x0400 and 0x0800, triggers 255, thumbs `0xFFC0` (as Windows reports), Vibration 65535/65535 |
| `XInputEnable(bool)` | 1 | Store; while disabled `GetState` reports zeros (with a still-valid packet) and a zero rumble is sent once |
| `XInputGetBatteryInformation(user, type, *info)` | 3 | `BATTERY_TYPE_WIRED` (1), `BATTERY_LEVEL_FULL` (3). 1_3/1_4 only |
| `XInputGetKeystroke(user, reserved, *keystroke)` | 3 | From `host_pad_next_event`: VirtualKey `VK_PAD_A` 0x5800 … (see below), Flags `XINPUT_KEYSTROKE_KEYDOWN` 1 / `KEYUP` 2, UserIndex 0. `ERROR_EMPTY` (4306) when there are none. 1_3/1_4 only |
| `XInputGetDSoundAudioDeviceGuids(user, *render, *capture)` | 3 | Zero GUIDs, `ERROR_SUCCESS` for user 0. 1_3 and 9_1_0 |
| `XInputGetAudioDeviceIds(user, render, *rlen, capture, *clen)` | 5 | Lengths 0, `ERROR_SUCCESS`. 1_4 only |

**`XInputGetState`:**
- User 0, when served (`host_pad_mode() == 2 && (host_pad_native_apis() & 2)`): write `XINPUT_STATE` (16 bytes): `dwPacketNumber` = the host packet, then `XINPUT_GAMEPAD`, and return `ERROR_SUCCESS` (0).
- Any other user, or not served: `ERROR_DEVICE_NOT_CONNECTED`.

**Button mapping:**

| Pad | XInput bit |
|---|---|
| hat up | DPAD_UP 0x0001 |
| hat down | DPAD_DOWN 0x0002 |
| hat left | DPAD_LEFT 0x0004 |
| hat right | DPAD_RIGHT 0x0008 |
| start | START 0x0010 |
| select | BACK 0x0020 |
| l3 | LEFT_THUMB 0x0040 |
| r3 | RIGHT_THUMB 0x0080 |
| l1 | LEFT_SHOULDER 0x0100 |
| r1 | RIGHT_SHOULDER 0x0200 |
| cross | A 0x1000 |
| circle | B 0x2000 |
| square | X 0x4000 |
| triangle | Y 0x8000 |

**Axes:** triggers are l2/r2 (0..255). `sThumbLX = lx`, `sThumbLY = -ly` (XInput Y is up), `sThumbRX = rx`, `sThumbRY = -ry`. Clamp −32768..32767.

**Keystroke VKs:**

| Pad | VK |
|---|---|
| cross | 0x5800 |
| circle | 0x5801 |
| square | 0x5802 |
| triangle | 0x5803 |
| r1 | 0x5804 |
| l1 | 0x5805 |
| l2 | 0x5806 |
| r2 | 0x5807 |
| dpad up / down / left / right | 0x5810 / 0x5811 / 0x5812 / 0x5813 |
| start | 0x5814 |
| select | 0x5815 |
| l3 | 0x5816 |
| r3 | 0x5817 |

Axis events aren't reported as keystrokes. Keep a per-process `last_sequence` for keystrokes.

- [ ] **Step 1: Write the failing tests** in `pad_tests.cpp`. Call the shims through the import table (look up `imports_serves_module("xinput1_3.dll")` and the trampoline mechanism `dx_tests` uses for other DLL exports):
  - User 1 → 1167.
  - Mode 1 → 1167.
  - Mode 2: cross + hat up → buttons `0x1001`; ly −1 → `sThumbLY == 32767`.
  - The packet changes with the state.
  - `SetState(0, {1000, 2000})` → the fake records (1000, 2000).
  - `XInputEnable(0)` → the state reads as zero and the fake records (0, 0).
  - Keystroke: after a cross press event → VK 0x5800, flags 1. Then `ERROR_EMPTY`.
  - `imports_serves_module("xinput9_1_0.dll")` is true.
- [ ] **Step 2:** Run and confirm failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `pad_tests` and `tools/test.py --native`. Expected: PASS.
- [ ] **Step 5:** Commit: `XInput: serve the virtual pad as controller 0`

---

## Phase 5: phones and portrait

### Task 17: Phone layouts and form-factor switching

**Files:**
- Modify: `host/controls/builtin_layouts.cpp`, `host/controls/controls_host.cpp`, `host/tests/controls_tests.cpp`

**Built-in phone layouts:**
- **`phone-landscape`** (about 844×390 pt screens):
  - `pad`: the tablet pad scaled for a short screen.
    - Sticks: radius 70.
    - Face buttons: size 48, diamond offsets ×0.75.
    - Dpad: size 100 at x 170, y 24.
    - L/R shoulders: w 80, h 34, at y 12 and y 52.
    - Select, PS and Start: at `bottom-center`, y 8, w 56, h 26.
  - `keys`: grid 10×4 split into two halves of 5×4, key 30, gap 3. Letters only, plus Shift, Ctrl, Alt, Space, Enter, Bksp, Esc and the arrows (list the keys explicitly in the JSON; the spec leaves the choice to the plan). Function keys are left out; the F-keys go in the `pad+keys` layout's top row instead. One cycle toggle.
  - `pad+keys`: both sticks, the face diamond (size 42), and a top strip of 10 keys (Esc, F1–F5, Tab, Enter, Space, Bksp) at `top-center`.
- **`phone-portrait`** (anchors resolve in the controls area below the game):
  - `pad`:
    - Left stick: `center-left`, x 24, radius 80.
    - Face diamond (size 60): `center-right`.
    - Dpad: `bottom-left`, size 110.
    - Shoulders: a row at `top-left`/`top-right`, y 8.
    - Select, PS, Start: `bottom-center`.
    - Right stick: `bottom-right`, radius 60.
  - `keys`: a full 10-column QWERTY in 4 rows plus a modifier row (grid 10×5, key 34, gap 3, `anchor center`), with no split.
  - `pad+keys`: the `pad` controls in the top half of the area (anchors `top-*`) and a 10×3 letter grid anchored `bottom-center`.
  - Every `phone-portrait` layout has `"safe_inset": true`.

**`controls_host`:**
- Recomputes `form_for(dw, dh, scale)` every pump and reloads the layout when it changes. Releasing held controls happens through `router.set_layout`.
- Calls `binding.release_all` on a change.
- Group-hidden bits are per layout name, not per form. Clamp them to the number of groups.

- [ ] **Step 1: Write the failing tests:**
  - Every `(name, form)` built-in parses.
  - Every control fits inside its reference area:
    - Phone landscape: 844×390 pt with safe insets of 47 on each side.
    - Phone portrait: the controls area of 390 × (844 − 390·3/4 − 59) pt, taking a 4:3 game at full width below a 59 pt top inset.
  - No two pad controls overlap.
  - Every portrait key is at least 30 pt wide.
- [ ] **Step 2:** Run and confirm failure. Implement. Run the tests.
- [ ] **Step 3:** Commit: `Controls: phone layouts in landscape and portrait`

---

### Task 18: Portrait presentation and pointer mapping

**Files:**
- Modify: `host/present.h`, `host/present_thread.cpp`, `host/input_gate.cpp`, `host/sdl/main.cpp` (`view_point_to_drawable`, lines ~146–200), `host/window_presentation.h` (read it first; the "any resolution" display code lives around here), `host/controls/controls_host.cpp`, `host/controls/overlay_view.cpp`, `host/tests/host_tests.cpp` (or wherever presentation placement is tested; find it with `grep -rn "letterbox\|placement\|fit" host/tests | head`)

**Interfaces:**
- Produces:
```cpp
// host/present.h
struct HostGameRect { int x, y, w, h; };
// Where the game image is drawn inside a drawable of dw x dh, for a game frame of gw x gh.
// Landscape (dw >= dh): exactly today's placement. Portrait: full width, aspect kept, top of the
// safe area (safe_top pixels down).
HostGameRect host_present_game_rect(int dw, int dh, int gw, int gh, int safe_top);
void host_present_set_safe_top(int pixels);
```

**Steps for the implementer:**
1. **Find today's placement.** Read `present_thread.cpp` and `window_presentation.h` and locate where the game frame's destination rectangle is computed (search for `viewport`, `dst`, `scale_to`, `aspect`). Extract that computation into `host_present_game_rect` unchanged for `dw >= dh`. **Landscape output must be bit-identical:** add a test that compares the new function with the old inline computation over a table of sizes (16:9, 4:3 and 21:9 drawables × 640×480, 800×600 and 3840×2160 games) before switching the call site.
2. **Portrait branch:** `w = dw`, `h = lround(dw * gh / (double)gw)`, `x = 0`, `y = safe_top`. When `h > dh - safe_top`, fall back to the landscape rule.
3. **Pointer mapping.** Every mapping from window or drawable pixels to guest coordinates must go through the same rect: the gate's hit testing and `view_point_to_drawable` consumers. Find them with `grep -rn "drawable_w\|fallback_layout" host`. Where the gate uses a stored layout, publish the rect alongside it. **Test:** in portrait 1170×2532 with a 640×480 game and safe_top 141, a drawable point at (585, 141 + 438) maps to guest (320, 240).
4. **Controls area.** `controls_host` sets `Screen.controls_area = {0, rect.y + rect.h, dw, dh - (rect.y + rect.h) - safe_bottom}` in portrait, and leaves it empty otherwise. `make_view` copies it, and the overlay fills it with `12,14,18,255` before drawing controls, so the area below the game is not garbage.
5. **Gestures stay on the game image.** `TouchMapper` edge scrolling uses the game rect's edges in portrait. Pass the rect's point bounds to `g_touch.set_bounds` and offsets; if `TouchMapper` has no offset concept, add `set_origin(x, y)` with a test in `input_touch_tests.cpp`. Touches in the controls area are claimed by the router, since Task 6 of the spec says every controls-area touch is claimed: `Router::set_claim_area(Rect)`.

- [ ] **Step 1:** Write the landscape-equality test and the portrait mapping test. Run and confirm failure.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** Run `tools/test.py --native`. All existing presentation and input tests must pass unchanged.
- [ ] **Step 4:** Commit: `Present: pin the game to the top in portrait and map touches through one rectangle`

---

### Task 19: Orientation on phones

**Files:**
- Modify: `host/Info-ios.plist.in` (lines 21–24), `cmake/IosBundle.cmake` (lines 17–18), `host/sdl/platform_ui_ios.mm` (line 43), `platform/android/app/src/main/AndroidManifest.xml.in` (line 15), `platform/android/app/src/main/java/dev/recompkit/RecompActivity.java`, `tools/tests/test_build.py` (line 48)

**Changes:**
- **`Info-ios.plist.in`:** `UISupportedInterfaceOrientations` (iPhone) lists `UIInterfaceOrientationPortrait`, `LandscapeLeft` and `LandscapeRight`. The `~ipad` key stays landscape-only.
- **`IosBundle.cmake`:** add the matching iPhone key `XCODE_ATTRIBUTE_INFOPLIST_KEY_UISupportedInterfaceOrientations~iphone` with `"UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight"`.
- **`platform_ui_ios.mm`:** `SDL_SetHint(SDL_HINT_ORIENTATIONS, UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "LandscapeLeft LandscapeRight" : "LandscapeLeft LandscapeRight Portrait");`
- **`AndroidManifest.xml.in`:** `android:screenOrientation="fullUser"`, and `android:configChanges` must include `orientation|screenSize|screenLayout|keyboardHidden` (check what is there already).
- **`RecompActivity.java`:** in `onCreate`, before `super.onCreate`, if `getResources().getConfiguration().smallestScreenWidthDp >= 600`, call `setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE)`.
- **`platform_ui_desktop.cpp`:** it sets `SDL_HINT_ORIENTATIONS` at line 51 for Android. Make it `"LandscapeLeft LandscapeRight Portrait"`; the activity locks tablets.
- **`test_build.py`:** expect `fullUser`.
- **Rotation:** `controls_host` releases everything on a form change (Task 17). `main.cpp` already handles `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`. Also call `touch_release_all()` there when the aspect flips between portrait and landscape.

- [ ] **Step 1:** Update `test_build.py` first, run `.venv/bin/python tools/test.py`, and confirm the failure.
- [ ] **Step 2:** Make the changes.
- [ ] **Step 3:** Run `tools/test.py` and `tools/test.py --native`. Build `ios-stub`/`android-stub` if the toolchains are present, and report which ran.
- [ ] **Step 4:** Commit: `Phones rotate; tablets stay landscape`

---

## Phase 6: editor

### Task 20: Editor model

**Files:**
- Create: `host/controls/editor.h`, `host/controls/editor.cpp`
- Modify: `host/CMakeLists.txt`, `host/tests/controls_tests.cpp`

**Interfaces:**
- Consumes: `Layout`, `Screen`, `control_rect`, `hit_test`, `LayoutStore`, `MappedTable`, `write_mapped`, `target_name`.
- Produces:
```cpp
namespace controls {
enum class Tool { None, Add, Delete, Bind, Stick, Snap, Layout, Reset, Done };
struct ToolbarItem { Tool tool; const char *label; Rect rect; };
struct PickerItem { std::string label; std::string value; Rect rect; };
class Editor {
  public:
    // Starts on a copy of `layout`; `mapped` is the current binding table; `native` true hides mapped targets.
    void open(const Layout &layout, Form form, const MappedTable &mapped, bool native, bool snap);
    bool is_open() const;
    void set_screen(const Screen &s);
    // Pointer input in drawable pixels. Two fingers on the selected control pinch it.
    void finger_down(int64_t id, double px, double py);
    void finger_motion(int64_t id, double px, double py);
    void finger_up(int64_t id);
    void wheel(double notches);            // desktop: resize the selected control
    const Layout &layout() const;          // the edited copy
    const MappedTable &mapped() const;
    int selected_group() const; int selected_control() const;
    const std::vector<ToolbarItem> &toolbar() const;
    const std::vector<PickerItem> &picker() const; // empty when no picker is open
    const std::vector<Rect> &guides() const;       // snap guide lines (1-px-wide rects)
    bool snap() const;
    // Results the host acts on; each returns true once.
    bool take_save();      // Done: host saves layout() for form and mapped() if changed, then closes
    bool take_reset();     // Reset: host deletes the user copy, reloads, reopens
    bool take_cancel();    // (reserved: back gesture) host closes without saving
    uint32_t generation() const;
};
}
```

**Behaviour:**
- **Toolbar:** a row of 9 items, each 72×36 pt with 6 pt gaps.
  - Landscape: centred at the top of the safe area.
  - Portrait: at the top of `controls_area`.
- **Picker:** a vertical list that opens below the toolbar, 200 pt wide, with 32 pt rows and up to 12 visible rows. It scrolls with a finger drag. Choosing an item applies it and closes the picker.
- **Tapping a control** selects it. **Tapping empty space** clears the selection.
- **Dragging a selected control** moves it:
  - For a non-grid control, update x/y. On release, rewrite the anchor to the nearest of the 9 anchor points (by the control centre's position in thirds of the area) and recompute x/y so the rect is unchanged.
  - For a grid key, move the whole group: update the group's x/y and re-anchor it the same way.
- **Snapping, when on:**
  - Control edges and centres snap to the 10 pt grid, or to any other control's edges or centre within 6 pt; the other control wins over the grid.
  - `guides()` holds the lines currently snapped to.
- **Pinch:** a second finger scales the selected control's w/h (or radius) by the distance ratio, clamped to 24–240 pt, and snaps to 2 pt steps. For grid keys the pinch scales `grid.key`, clamped to 24–60.
- **Add:** opens the picker with Key, Button (each PS button), Dpad, Left stick, Right stick, Toggle (next layout) and Action (settings). The new control is placed at the centre of the area, in a group named `custom`, which is created if missing. It is selected.
- **Delete:** removes the selected control. The last control of a group removes the group, unless it is a grid group.
- **Bind**, depending on the selection:
  - Key: the picker lists every `scancode_name`.
  - Button in mapped mode: the targets `none`, `mouse_left/right/middle`, `wheel_up/down`, `key:<every name>` and `action:settings`. The choice changes `mapped().buttons[...]`.
  - Stick in mapped mode: the stick modes, which set `left`/`right`.
  - Button in native mode: every pad button name, which changes `control.button`.
  - Toggle: every layout name from the store plus `next`.
- **Stick:** cycles the selected stick through floating/fixed and dead zones 0.10/0.15/0.25.
- **Snap:** flips `snap()`. The host mirrors it into `CONTROLS_SNAP_ROW`.
- **Layout:** opens the picker with Duplicate (saves as `<name> copy`, adding a number if needed) and Rename.
  - Rename uses the system keyboard; this goes through the host: `bool take_rename(std::string *name)`, which the host fulfils via `SDL_StartTextInput` and `text(...)` events. Add `void text(const std::string &utf8)` and `void text_done()`.
  - Built-in names can't be renamed.
- **Reset:** sets `take_reset`.
- **Done:** sets `take_save`.

- [ ] **Step 1: Write the failing tests:**
  - **Opening:** `open` on the `pad` built-in at 2360×1640, scale 2. The toolbar has 9 items within the top 100 px.
  - **Selecting:** tapping ✕ selects it (the group and control indexes match).
  - **Dragging:** drag ✕ by (−400, −600) px with snap on, then release:
    - The rect moved by about (−400, −600) and its edges lie on the 20 px (10 pt × 2) grid.
    - The anchor changed to the region the centre ended in.
    - `control_rect` equals the rect before re-anchoring.
  - **Pinch:** a second finger at twice the distance → size ×2, clamped.
  - **Add:** Add then Button then Circle → a new control in `custom`, selected.
  - **Delete:** removes it, and `custom` is gone.
  - **Bind:** with ✕ selected in mapped mode, Bind then `key:Space` → `mapped().buttons[Cross]` is Key Space.
  - **Snap off:** a drag lands on unsnapped pixels.
  - **Done:** `take_save()` is true once.
  - **Rename:** Layout then Rename → `take_rename` is true; `text("mine")` then `text_done()` sets the layout name to `mine`.
- [ ] **Step 2:** Run and confirm failure. Implement. Run the tests.
- [ ] **Step 3:** Commit: `Controls: the layout editor model`

---

### Task 21: Editor in the app

**Files:**
- Modify: `host/controls/controls_host.{h,cpp}`, `host/controls/overlay.cpp`, `host/controls/overlay_view.cpp`, `host/sdl/main.cpp`, `mods/controls_settings.cpp` (the EDIT row is already there), `host/tests/controls_tests.cpp`

**Behaviour:**
- **Opening:** the editor opens on `mods_controls_take_edit_request()`, polled in `host_pump`, or on an `edit_layout` action.
  - On open, close the F10 page: find the close call next to `mods_page_open` in `mods/settings_page.cpp`.
  - Pause the game. Find how the page pauses: `grep -rn "mods_page_visible" host runtime mods` shows what `mods_page_visible()` gates. Add `bool mods_controls_editing()`, a flag in `controls_settings.cpp` that `controls_host` sets, and OR it into the same gate(s) that `mods_page_visible()` feeds, so the game is frozen exactly as it is while the page is open. If the page doesn't pause the simulation, report that and freeze input only: every finger, key and pad event goes to the editor, and the binding/router is disabled. The spec's pause becomes "input frozen", so note it for Task 22.
- **While editing:**
  - `host_finger_*` go to the editor.
  - Desktop mouse: `main.cpp` sends left-button down/up and motion to the editor as finger id −1, and the wheel to `editor.wheel`. Keys: Escape = Done; ignore the rest.
- **The view** (`make_view` with an `Editor*`):
  - Dim the whole drawable with `0,0,0,110`.
  - Draw the edited layout's controls at full opacity, the selected control with a 3 px `255,200,64` outline, the guides as `255,200,64,200` lines, and the grid dots (every 10 pt, `255,255,255,40`) when snapping is on.
  - Draw the toolbar and picker as key-style round rects with labels.
  - The overlay must draw the dimmer even where no control exists: its raster covers the whole drawable while editing.
- **Results:**
  - `take_save`: `store.save_user_copy(editor.layout(), form)`. If the mapped table changed, write `<profile>/controls/binding.txt` via `write_mapped`, keeping only the entries that differ from `RECOMP_CONTROLS_MAPPED`, and apply it to the binding. Mirror `snap` into the setting, close the editor, and reload the layout.
  - `take_reset`: `store.delete_user_copy(name, form)`, delete `binding.txt`, reload, and reopen the editor on the reloaded layout.
  - A rename or duplicate that changes the name: save under the new name, call `mods_controls_set_names(store.names())` and refresh the settings row ranges (add `mods_controls_refresh_names()`), then select the new name.
- **Text input:** `SDL_EVENT_TEXT_INPUT` goes to `editor.text`, and Return goes to `text_done` while a rename is pending.

- [ ] **Step 1: Write the failing test:** `make_view` with an open editor has `editing == true`, 9 toolbar entries and `selected` set after a tap. The view revision changes on a drag.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** Run `tools/test.py --native`.
- [ ] **Step 4: Manual check on the Mac** with `RECOMP_KEYPAD=1`: F10 → Edit controls → drag a button with the mouse → Done. Relaunch and confirm the new position persisted. Then Reset and confirm the default is back. Report the screenshots.
- [ ] **Step 5:** Commit: `Controls: edit layouts in the app`

---

### Task 22: Docs, spec sync, final verification and PR

**Files:**
- Modify: `CHANGELOG.md`, `docs/superpowers/specs/2026-09-17-touch-controls-design.md`, `docs/superpowers/specs/2026-09-13-keypad-design.md` (mark it superseded), `README.md` or `docs/` wherever `RECOMP_KEYPAD` or the keypad is documented (`grep -rn -i keypad docs README.md`)

- [ ] **Step 1: Sync the spec with what was built:**
  - Grid groups, `stack_on`, `safe_inset`, `label_off`.
  - Non-grid groups don't claim gaps.
  - The `hidden` and `size` settings.
  - `binding.txt` in `k=v;` syntax instead of `binding.json`.
  - The `Vpad` uses a mutex.
  - The DI8 integration note for the `nfsmw` branch.
  - Whether the editor pauses the game or only freezes input.
- [ ] **Step 2: CHANGELOG.** Add a user-facing entry: the on-screen PS-style pad; editable layouts; physical controllers; rumble and haptics; phone layouts and portrait; `[controls]` in game.toml replacing `[touch] keypad`, which is still accepted.
- [ ] **Step 3: Run the full verification and paste the results into the PR body:**

```bash
.venv/bin/python tools/format.py --write && git diff --exit-code
.venv/bin/python tools/check_game_literals.py
.venv/bin/python tools/check_repo.py
.venv/bin/python tools/test.py
.venv/bin/python tools/test.py --compile-only
.venv/bin/python tools/test.py --native
```

  Also build `ios-stub` and `android-stub` if their toolchains are installed, and record which ran.
- [ ] **Step 4:** Commit: `Docs: touch controls`
- [ ] **Step 5: Push and open the PR.** Only after the controller confirms with the user that pushing is wanted:
  - `git push -u origin touch-controls`
  - `gh pr create --base main --title "Touch controls: on-screen pad, editable layouts, controllers, native input, phones"` with a body listing the six steps, the verification output, the manual checks done, and the ones still needed on the iPad, iPhone and Android devices.
- [ ] **Step 6:** Watch CI (`gh pr checks --watch`) and fix failures on the branch.

---

## Self-review notes

- **Spec coverage:**
  - §4 concepts: Tasks 2, 9, 17.
  - §5 files, sources and settings: Tasks 2–4 and 6.
  - §6 routing and auto-hide: Tasks 5, 12, 18.
  - §7.1 keys: Tasks 5 and 7.
  - §7.2 vpad: Tasks 9 and 12.
  - §7.3 DirectInput joystick: Task 15.
  - §7.4 XInput: Task 16.
  - §7.5 mapped binding: Tasks 6 and 10.
  - §7.6 rumble and haptics: Task 13.
  - §8.1 drawing: Tasks 7, 8, 11.
  - §8.2 portrait: Tasks 17–19.
  - §9 editor: Tasks 20 and 21.
  - §12 tests: in each task.
  - §11's one PR: Task 22.
- **Known deviations**, recorded for Task 22: `binding.txt` instead of `binding.json`, a mutex instead of a lock-free `Vpad`, grid groups and `stack_on` as model additions, and gaps claimed only by grid groups.
- **Interface consistency:**
  - `mods_controls_init(default_layout)` and `mods_controls_set_names(names)`: Task 6 step 4 is authoritative.
  - `ControlsView` gains `backdrops` in Task 7.
  - `TouchAction::Wheel` is added in Task 10 and used in Task 10's `main.cpp` change.
