# Touch controls design: a data-driven on-screen gamepad and keyboard

Date: 2026-09-17
Status: built on branch `touch-controls` (all six steps of section 11).
This document describes what the branch does, not what was first proposed:
where the build settled somewhere else, the built behaviour is what is
written here, with a sentence of reasoning.
Parent spec: `2026-09-13-recomp-kit-design.md`; supersedes
`2026-09-13-keypad-design.md` in full

## 1. Goal

Give every game the kit builds two on-screen control sets a player can
reshape: a PlayStation-style gamepad and a keyboard. Both are layouts of one
widget system, stored as JSON, shipped per game and editable on the device.
The gamepad drives a game through a real DirectInput joystick and XInput
controller when the game reads one, and through keys and mouse when it does
not. A physical controller feeds the same path as the on-screen pad and
receives the game's rumble. Tablets, phones (including phones in portrait)
and desktops are all covered.

## 2. Decisions taken in conversation

| Topic | Decision |
|---|---|
| Architecture | One widget system for pad and keyboard (approach A). Not a second overlay stack, not scripting. |
| Pad output | Per game: `native` (DirectInput joystick + XInput) or `mapped` (keys and mouse). |
| Customization | Layout files plus an in-game editor opened from F10: move, resize, add, remove, rebind; save; reset. |
| Pad and keyboard together | Whole-layout switching: one layout on screen at a time (e.g. `pad`, `keys`, `pad+keys`), with a toggle control to cycle. |
| Mapped stick defaults | Left stick → arrow keys. Right stick → mouse cursor. ✕ → left click. |
| Edited layouts | Per game, in that game's profile. No global layouts. |
| Physical controller connected | On-screen pad-only layouts auto-hide; F10 can force them back. Key layouts stay. |
| Stick default | Floating. It can be set to fixed per stick in the editor. |
| Devices | iPad, iPhone, Android phones and tablets, and desktop (Mac, Windows, Linux) in the first version. |
| XInput | Included now: `xinput1_3`, `xinput1_4` and `xinput9_1_0`, fed by the same virtual pad. |
| Rumble | `XInputSetState` and game rumble are forwarded to the physical controller through SDL, or to the phone or tablet's haptic motor when no controller is connected. |
| Button haptics | A light tap when an on-screen control is pressed; on by default, with a toggle on F10. |
| Look | DualSense-styled: shaded buttons, a rimmed stick base, PS-coloured ✕○□△. |
| Portrait | Phones only. Tablets stay landscape. In portrait the game is pinned to the top at full width and the controls fill the space below. |
| Editor | Snaps to a 10 pt grid and to other controls' edges; snapping can be switched off. |
| Branch | `touch-controls` off the current kit `main` (32bb542), in its own worktree; one PR. |

## 3. Non-goals

- winmm `joy*` functions. They are added when a game imports them, on the
  same virtual pad.
- DirectInput force-feedback effects (`CreateEffect`); the device reports
  no FF support. Rumble reaches games through XInput only.
- Text entry. The four-finger toggle for the system keyboard stays.
- Global or shared layouts, themes and user images.
- More than one virtual pad (local multiplayer). XInput reports only user
  index 0.
- Portrait on tablets.
- DualSense-specific features: touchpad, adaptive triggers, lightbar, gyro.

## 4. Concepts

- **Control**: one on-screen element. Kinds:
  - `key`: holds a scancode while touched. Modifier scancodes use the
    existing Off/Held/Latched/Locked machine (`keypad_modifiers`).
  - `button`: a pad button (`cross`, `circle`, `square`, `triangle`, `l1`,
    `r1`, `l2`, `r2`, `l3`, `r3`, `select`, `start`, `ps`). `l2`/`r2` also
    set their trigger axis to full while held.
  - `dpad`: four directions plus diagonals from the touch angle, in 45
    degree sectors with a 25% dead zone at the centre. Sliding between
    sectors changes the held direction.
  - `stick`: `left` or `right`, with `mode` `floating` (the default) or
    `fixed`. A floating stick recentres on the first touch inside its
    zone; a fixed stick stays where it is drawn. Output is the offset from
    centre divided by `radius`, clamped to the unit circle, with a radial
    dead zone (default 0.15) rescaled so output starts at 0.

    A stick has two sizes, because the area a thumb may land in is not the
    distance the knob travels. `Control.radius` is the knob travel (and the
    drawn base radius); the control's `w`/`h` are its *zone*, the rectangle
    the hit test uses and a floating base is clamped inside. `"zone": [w, h]`
    sets the zone, and it defaults to `2 * radius`. Without the split a
    floating stick would be a no-op, since its zone would be exactly the
    base it recentres inside.
  - `toggle`: shows or hides a named group, switches to a named layout, or
    cycles to the next layout (`"target": "next"`). `label_off` is the text
    drawn while its target group is hidden (`label` is the text while it is
    shown), so one control reads HIDE/KEYS as the keypad tabs do.
    `stack_on: "<group>"` parks the toggle directly on top of that group
    while the group is visible, and at the group's anchor edge while it is
    not, which is how a tab follows the half it opens. A stacked toggle
    cannot be moved vertically in the editor; its y comes from the group.
  - `action`: a host action (`settings`, `system_keyboard`, `edit_layout`).
- **Group**: a named set of controls that toggles together (the two keypad
  halves are groups `left` and `right`). A group is either
  - a **grid group** (`"grid": {"cols", "rows", "key", "gap"}`), which has
    its own anchor and offset and lays its controls out by `col`/`row`/`span`
    in a fixed box. A grid group is drawn on a backdrop, and it claims the
    gaps between its keys (section 6), as the keypad halves do; or
  - a plain group, whose controls each carry their own anchor and offset.
    Its rectangle is just the union of its controls, it has no backdrop and
    it claims no gaps. The pad layouts are built from plain groups, so the
    game stays touchable between the buttons.
- **Layout**: a named list of groups. Exactly one layout is active. The
  built-in and default game layouts are `pad`, `keys` and `pad+keys`, and a
  toggle control in each cycles between them.
- **Form factor**: `tablet`, `phone-landscape` or `phone-portrait`,
  chosen from the screen's smallest side (phone below 600 pt) and the
  current orientation. Desktop counts as `tablet`.
- **Virtual pad (vpad)**: 13 buttons, a 4-direction hat and six axes (LX,
  LY, RX, RY in [-1, 1]; L2, R2 in [0, 1]). On-screen controls and
  physical controllers write to it; the binding stage reads from it.
- **Binding stage**: turns the vpad into what the game sees (section 7).

## 5. Layout files

JSON, one layout per form factor per file. Geometry is in points, anchored
to a screen edge or corner inside the safe area, so one file fits every
screen of its form factor.

```json
{
  "version": 1,
  "name": "pad+keys",
  "opacity": 0.7,
  "scale": 1.0,
  "safe_inset": true,
  "groups": [
    { "id": "main", "visible": true, "controls": [
      { "kind": "stick", "stick": "left", "mode": "floating",
        "anchor": "bottom-left", "x": 40, "y": 40,
        "radius": 44, "zone": [140, 140] },
      { "kind": "button", "button": "cross",
        "anchor": "bottom-right", "x": 90, "y": 40, "size": 56 },
      { "kind": "toggle", "target": "next", "label": "KEYS",
        "anchor": "bottom", "x": 0, "y": 8, "w": 64, "h": 20 }
    ]},
    { "id": "left", "visible": true,
      "grid": { "cols": 6, "rows": 4, "key": 36, "gap": 4 },
      "anchor": "bottom-left", "x": 8, "y": 8,
      "controls": [
        { "kind": "key", "scancode": "F5", "label": "SAVE",
          "col": 0, "row": 0, "span": 2 }
      ]},
    { "id": "left-tab", "visible": true, "controls": [
      { "kind": "toggle", "target": "left", "label": "HIDE",
        "label_off": "KEYS", "stack_on": "left",
        "anchor": "bottom-left", "x": 8, "y": 0, "w": 44, "h": 18 }
    ]}
  ]
}
```

- `anchor` is one of the four corners, the four edge midpoints or `center`.
  `x`/`y` are the inset from that anchor to the control's matching point.
  In a grid group the group carries the anchor and offset, and each control
  carries `col`, `row` and `span` instead.
- Size is `size` for round or square controls, or `w`/`h`. A stick's knob
  travel is `radius` and its touch zone is `zone` (see section 4).
- `scale` multiplies sizes but not offsets, and the `size` setting
  multiplies it again. `safe_inset` (default true) decides whether anchors
  resolve inside the screen's safe area or against the whole drawable.
- `scancode` is a name from the existing `KeypadScan` table ("A", "F5",
  "LShift"). `label` defaults to the key name.
- **Portrait anchoring.** In `phone-portrait` the anchors refer to the
  *controls area*, the part of the screen below the game image (section 8.2),
  not the whole screen. A `top-*` anchor there means just below the game.
- Unknown fields are ignored and unknown `kind`s are skipped with a log
  line, so files from a newer kit load in an older one.
- A file that fails to parse is ignored and the next source is used; the
  error is logged once and shown on the F10 page.

File names are `<name>.<form>.json` (e.g. `pad.phone-portrait.json`), with
`<name>.json` as the fallback for any form factor. The loader tries, first
match wins:

1. `<profile>/controls/<name>.<form>.json`, then
   `<profile>/controls/<name>.json`: the player's edited copies.
2. `<game data>/controls/<name>.<form>.json`, then `<name>.json`: shipped
   by the game repo (its `layouts/` directory, copied into the app by
   `tools/copy_layouts.py`, which `tools/build.py` and
   `tools/package_desktop.py` call).
3. Kit built-ins for `keys`, `pad` and `pad+keys`, one for each of the
   three form factors. The tablet `keys` built-in reproduces today's split
   keypad exactly. A layout with no file for the running form factor falls
   back to the tablet one.

**Android.** An APK has no readable resource directory, so resources are
read from the app's data folder: `tools/build.py` copies `layouts/` into
the APK under `assets/controls`, the activity unpacks the assets into the
app's external files directory on first run, and the Android entry point
points `RECOMP_RESOURCES_DIR` at that folder. Every bundled resource, not
only layouts, is reachable that way.

The editor saves to the form-specific name, so an edit on a phone in
portrait does not change the landscape layout.

**Settings** (mod settings store, shown on F10):

| Key | Values |
|---|---|
| `host.controls/layout` | The active layout's index in the discovered-layout list. One index past the last name is the **Hidden** slot: nothing is drawn. |
| `host.controls/size` | 0, 1 or 2 — small, normal, large: the scale factors below |
| `host.controls/opacity` | 20–100 (%), default 100 |
| `host.controls/haptics` | 0/1, default 1 |
| `host.controls/pad_with_controller` | 0 auto-hide (default), 1 always show |
| `host.controls/snap` | 0/1, editor snapping, default 1 |
| `host.controls/hidden` | A bit per group of the active layout, hidden by its toggle (16 bits) |

The layout row's maximum is dynamic: the host discovers the layout names
and hands them to `mods_controls_set_names` before `mods_controls_init`
runs, and the declared range is re-set whenever the names change.

"Edit controls" is a row of the same page but is never declared or
persisted; it opens the editor (section 9). The native Options Display tab
shows layout, size, opacity, haptics and Edit controls; `pad_with_controller`
and `snap` are on the F10 fallback page (and snap also on the editor's
toolbar), because the Display tab holds eight rows.

The existing `host.keypad/left`, `/right` and `/size` values migrate on
first run:

- `left` and `right` become the visibility of the matching groups in the
  `keys` layout.
- `size` becomes a scale factor of 0.89, 1.0 or 1.11 applied to that
  layout.

Only `pad`, `keys` and `pad+keys` can be the default at first launch, so
the index cannot point at a layout that no longer exists. If an index is
out of range at load, the game's `default_layout` is used. After the
migration the `host.keypad/*` keys are no longer written.

## 6. Touch routing

`ControlsRouter` replaces the keypad branch of the finger handler in
`host/sdl/main.cpp`:

- **Finger down:** hit-test the visible controls of the active layout,
  topmost first.
  - A hit claims the finger for that control until it lifts or is
    cancelled.
  - A miss goes to `TouchMapper` as today.
  - The space between the keys of a visible **grid** group is claimed and
    does nothing, as the keypad's gaps do now. A plain group claims nothing
    but its controls, so the game stays reachable between the pad's
    buttons.
  - In portrait, the router claims every touch in the controls area, so
    the gesture mapper only sees touches on the game image.
- **Finger motion** goes to the control that owns the finger. Sticks and
  the dpad use it; keys and buttons ignore it, so a finger that slides off
  still holds.
- Several fingers can own several controls at once.
- On press, if `haptics` is on, the router emits a light haptic event
  (section 7.6).
- **Auto-hide:**
  - A hardware keyboard hides layouts made only of `key` controls, as the
    keypad does today.
  - A connected physical controller hides layouts made only of pad
    controls, unless `pad_with_controller` is 1.
  - Layouts that mix both kinds stay visible.
  - When the active layout is hidden, its toggle is still drawn as a small
    corner tab so the player can switch.

## 7. Output

### 7.1 Keys and host actions

`key` controls emit the same key down/up the keypad does now, through
`handle_key`, so DirectInput, `WM_KEYDOWN`/`WM_CHAR` and
`GetAsyncKeyState` all see them. `action` controls call the existing host
entry points.

### 7.2 The vpad

`host/controls/vpad.{h,cpp}` holds a state snapshot and an edge queue,
both under one mutex rather than lock-free. The input thread writes them;
the game thread reads the snapshot when it polls and drains the queue for
buffered reads. A pad poll happens a few hundred times a second at most,
and the state is wider than one word, so a lock-free design would have
bought nothing but a harder invariant. Sources:

- On-screen `button`, `dpad` and `stick` controls.
- The first physical controller, opened through `SDL_Gamepad` with
  hot-plug. SDL's standard mapping already names DualSense, Xbox and MFi
  buttons, so the kit has no per-controller tables. On iOS this includes
  MFi and DualSense through GameController.framework, which SDL wraps.

When both sources are active they are merged: buttons are ORed, and for
each axis the value with the larger magnitude wins.

### 7.3 `native`: DirectInput joystick

`dx/dinput.cpp` gains a joystick device kind, present when
`[controls] pad = "native"`:

- **Enumeration.** `EnumDevices` lists one `DI8DEVTYPE_GAMEPAD` instance
  ("Recomp Virtual Pad", with fixed instance and product GUIDs) for the
  joystick classes and filters (`DI8DEVCLASS_GAMECTRL`, `DIEDFL_ATTACHEDONLY`).
  DirectInput 7 and older callers see `DIDEVTYPE_JOYSTICK`.

  **DirectInput 8.** The kit's `main` serves DirectInput up to version 7
  only, because no game on `main` asks for 8. The version 8 class and type
  matching is written and unit-tested all the same, in
  `joy_enum_matches(devtype_filter, di_version)`, so the NFSMW branch — which
  does add `IDirectInput8` — calls it from its own `EnumDevices` without a
  second implementation.
- **`CreateDevice`** accepts that instance GUID and `GUID_Joystick`.
- **Data formats.** `SetDataFormat` accepts `DIJOYSTATE`, `DIJOYSTATE2` and
  any custom format whose objects map to the objects below.
- **Objects.** `EnumObjects` and `GetObjectInfo` report X, Y, Z, Rz, Rx, Ry,
  one POV and 13 buttons.
- **Properties.** `Get/SetProperty` support `DIPROP_RANGE`,
  `DIPROP_DEADZONE`, `DIPROP_SATURATION` and `DIPROP_BUFFERSIZE`.
  `GetCapabilities` reports no force feedback.
- **State.** `GetDeviceState` scales the vpad to the set range, and
  `GetDeviceData` drains the edge queue. `Poll` returns `DI_OK`.
  `Acquire`/`Unacquire` behave as they do for the mouse.
- **Default object order** follows DualShock on DirectInput:
  - Axes: LX→X, LY→Y, RX→Z, RY→Rz, L2→Rx, R2→Ry.
  - Buttons 0–12: square, cross, circle, triangle, L1, R1, L2, R2, select,
    start, L3, R3, PS.
  - A game expecting another order gets a `[controls.native]` remap table
    in `game.toml`.

With `pad = "mapped"` or `"off"`, `CreateDevice` behaves as it does today.

### 7.4 `native`: XInput

New `dx/xinput.cpp` registers import tables for `xinput1_3.dll`,
`xinput1_4.dll` and `xinput9_1_0.dll`, so both static imports and
`LoadLibrary` (through `imports_serves_module`) resolve.

| Function | Behaviour |
|---|---|
| `XInputGetState` | User 0: vpad as `XINPUT_GAMEPAD`, with `dwPacketNumber` incremented whenever the state changes. Users 1–3: `ERROR_DEVICE_NOT_CONNECTED`. |
| `XInputSetState` | User 0: forwards the two motor speeds to rumble (7.6); returns `ERROR_SUCCESS`. |
| `XInputGetCapabilities` | Gamepad subtype, all buttons and both motors present. |
| `XInputEnable` | While disabled, `GetState` reports neutral input and rumble stops. |
| `XInputGetBatteryInformation` | Wired, full (1_3/1_4). |
| `XInputGetKeystroke` | Built from the edge queue (1_3/1_4). |
| `XInputGetDSoundAudioDeviceGuids`, `XInputGetAudioDeviceIds` | No audio device. |

- Mapping: ✕→A, ○→B, □→X, △→Y, L1/R1→LB/RB, select→BACK,
  start→START, L3/R3→thumbs, dpad→DPAD.
- Sticks are scaled to ±32767 with Y up. Triggers are scaled to 0–255.
- PS has no public XInput bit and is not reported.
- **The DLLs are always registered**, whatever `pad` is. With
  `pad != "native"` every entry point answers
  `ERROR_DEVICE_NOT_CONNECTED` (and `XInputSetState` does nothing), so the
  game still takes its no-controller path. Leaving the imports unregistered
  was worse: a static import nothing has registered gets a generic stub
  whose argument count is wrong, which corrupts the stack on a `__stdcall`
  return, and a real Windows machine always has an XInput redistributable
  to import from.

### 7.5 `mapped`: keys and mouse

`host/controls/binding.{h,cpp}` evaluates a table from `game.toml` on each
input tick:

```toml
[controls]
default_layout = "pad"      # "pad" | "keys" | "pad+keys" | "hidden"
pad = "mapped"              # "native" | "mapped" | "off"

[controls.mapped]
left_stick = "arrows"       # "cursor" | "arrows" | "wasd" | "scroll" | "wheel" | "none"
right_stick = "cursor"
cursor_speed = 900          # points per second at full deflection
cross = "mouse_left"
circle = "mouse_right"
square = "key:Space"
triangle = "key:Tab"
l1 = "key:PageUp"
r1 = "key:PageDown"
l2 = "mouse_middle"
r2 = "key:LShift"
start = "key:Escape"
select = "key:F10"
dpad = "arrows"
```

Those values are also the kit defaults for any entry a game leaves out.

- **Button targets:** `key:<name>`, `mouse_left|right|middle`,
  `wheel_up|down`, `action:<host action>` or `none`.
- **`cursor`** moves the host cursor through the synthetic mouse path
  touch uses, so the absolute cursor, relative DirectInput motion and
  `WM_MOUSEMOVE` stay consistent. Speed follows the square of the
  deflection.
- **`arrows` and `wasd`** press their keys when an axis passes 0.5 and
  release them below 0.35, so the keys don't flicker.
- **`scroll`** emits `TouchMapper`'s two-finger pan action, one
  `kTouchPanStep` per tick at full deflection.
- **`wheel`** sends wheel notches at a rate that grows with deflection.
- **Player overrides.** The editor saves overrides for single entries to
  `<profile>/controls/binding.txt`, read on top of the `game.toml` table.
  The file is the same `k=v;k=v` syntax as the generated table and the
  `RECOMP_CONTROLS_MAPPED` switch, not JSON: one parser, one format to
  test. It holds only the entries the player changed.
- **Generated config.** `tools/gen_game_config.py` turns `[controls]` into
  the generated config header and replaces `RECOMP_TOUCH_KEYPAD_DEFAULT`.
  `[touch] keypad` is still read as an old spelling of `default_layout`
  ("auto" → "keys", "hidden" → "hidden") until every game repo is
  re-pinned.

### 7.6 Rumble and haptics

`host/controls/haptics.{h,cpp}` provides `rumble(low, high, ms)` and `tap()`:

- **`rumble`:** if a physical controller is open, it calls
  `SDL_RumbleGamepad`.
  - Otherwise, on a phone or tablet, it drives the device motor: Core
    Haptics on iOS (a continuous event whose intensity is the larger
    motor value); `Vibrator` with amplitude through JNI on Android.
  - On desktop with no controller it does nothing.
  - Rumble is refreshed while XInput keeps setting a non-zero state and
    stops on zero or `XInputEnable(FALSE)`.
- **`tap`:** a light impact on iOS (`UIImpactFeedbackGenerator`) and a short
  `EFFECT_TICK` on Android, when `host.controls/haptics` is 1. It does
  nothing on desktop.
- **Where the code lives:** the platform parts go in the existing
  `host/sdl/platform_ui_ios.mm`, a new `platform_ui_android.cpp`, and
  `platform_ui_desktop.cpp`.

## 8. Presentation

### 8.1 Drawing

`host/controls/overlay.{h,cpp}` replaces `keypad_overlay`:

- **Raster and upload.** Drawing is split into **layers**, one per layout
  group plus one for the portrait controls area, each a premultiplied RGBA
  texture covering only what that layer draws. Each layer carries a
  revision hashed from its own contents, and only the layers whose revision
  changed are re-rasterized and re-uploaded. One canvas for the whole
  drawable would mean repainting megabytes on every key press.
- **Blending.** The presenter blends the canvas where it blends the keypad
  today. `host_present_set_keypad` becomes
  `host_present_set_controls(ControlsView)`.

The DualSense-styled look, all rasterized on the CPU (no new shaders):

- **Face buttons:** dark discs with a radial highlight and a soft rim,
  carrying ✕ ○ □ △ drawn as anti-aliased strokes in the PlayStation
  colours (blue, red, pink, green). They brighten and shrink slightly
  while pressed.
- **Shoulder buttons:** rounded trapezoids with L1/R1/L2/R2 labels.
  Triggers fill from the bottom by their axis value.
- **Dpad:** four separate arrow-shaped buttons on a recessed disc.
- **Sticks:** a recessed base ring with a rim, and a concave knob with a
  highlight. The knob is a separate small quad drawn at the current offset,
  so a moving stick does not re-upload the canvas. A floating stick draws
  its base faint at its default spot and moves it to the touch.
- **Select, start and PS:** small pills, and a PS logo drawn from a
  simple path.
- **Keys:** the current keypad look (rounded rectangle, 6x8 font at 2x,
  lit when latched or locked), with the same shading as the buttons.
- **Opacity:** the setting scales the whole canvas.

To keep this bounded, a shading helper (`controls/raster.{h,cpp}`: disc,
ring, rounded rectangle, stroked path, each with radial and linear
gradients and anti-aliasing) is written once and unit-tested against
reference pixels.

### 8.2 Portrait on phones

- **Orientation lock:**
  - iOS: `UISupportedInterfaceOrientations` (iPhone) adds Portrait, while
    `~ipad` stays landscape. `platform_ui_ios.mm` sets
    `SDL_HINT_ORIENTATIONS` from the device idiom.
  - Android: the manifest's `screenOrientation` changes from `landscape` to
    `fullUser`. At startup the activity calls `setRequestedOrientation`
    with landscape when the smallest width is at least 600 dp, so tablets
    stay locked. `tools/tests/test_build.py` is updated to match.
- **Viewport:**
  - When the drawable is taller than it is wide, the presenter places the
    game image at full width, keeping its aspect ratio, pinned to the top of
    the safe area.
  - The rectangle below it is the *controls area* and is cleared to the
    overlay's background colour.
  - The viewport rectangle comes from one function,
    `host_present_game_rect()`. The pointer mapping (`view_point_to_drawable`
    and the gate), the gesture mapper's edge scroll and the "any
    resolution" display code all use it, so taps on the game image land on
    the right guest pixel.
- **Rotation:** a rotation mid-game resizes the drawable. The existing
  resize path rebuilds the swapchain, and the controls reload the layout
  for the new form factor. Held controls are released on rotation.

## 9. Editor

Reached from the F10 page ("Edit controls") or an `edit_layout` action.

- **Opening:** the editor **freezes input**, it does not pause the
  simulation. Every held key, button and stick is released as it opens, and
  nothing a finger does afterwards reaches the guest. The game keeps
  running behind the dimmed frame, as it does behind the F10 page, which
  does not pause it either. A physical controller in `native` mode also
  keeps reaching the guest while the editor is open: the editor intercepts
  touch, not the pad path.
- **Toolbar band.** While the editor is open, a band along the top of the
  anchor area is reserved for the toolbar and the anchor area shrinks by
  it, so no control can sit under the toolbar. Top- and centre-anchored
  controls therefore draw slightly lower while editing than in play; that
  is the accepted trade-off for the toolbar never covering what is being
  edited.
- The frame is dimmed and a 10 pt grid is drawn while snapping is on.
- **Moving and resizing:**
  - Drag moves a control. On drop its anchor becomes the nearest corner,
    edge or centre, so it stays in place across screen sizes.
  - Pinch resizes a control.
  - With snapping on, positions snap to the grid and to other controls'
    edges and centres within 6 pt. Guide lines show while snapping.
- **Toolbar:** at the top in landscape, and in the controls area in
  portrait.
  - Add: a palette of key, button, dpad, stick, toggle and action.
  - Delete: removes the selected control.
  - Bind: see below.
  - Stick: fixed or floating, and the dead zone.
  - Snap: on or off.
  - Layout: switch, duplicate, rename or delete a user layout.
  - Reset: restores the game default for this form factor.
  - Done.
- **Bind picker:**
  - Key controls: the key list.
  - Pad buttons and sticks in mapped mode: the targets from 7.5.
  - Pad buttons in native mode: the pad button list.
- **Saving:** Done writes `<profile>/controls/<name>.<form>.json` (and
  `binding.txt` if changed) through a temporary file and a rename, then
  leaves the editor. After a rename-save the old user file is deleted.
  Reset deletes the file.
- **Input while editing:** the editor's own hit test runs first, so
  controls never fire while being edited (see "Opening" above).
- **Desktop:** the mouse drives the editor, and the scroll wheel resizes.

## 10. Components

| File | Responsibility |
|---|---|
| `host/controls/layout.{h,cpp}` | Model, JSON load/save, source and form-factor lookup, anchor → drawable-pixel geometry (screen or controls area), hit test. SDL-free. |
| `host/controls/builtin_layouts.cpp` | Embedded `keys`, `pad`, `pad+keys` JSON for three form factors. |
| `host/controls/router.{h,cpp}` | Finger ownership, control state, modifier machine use, auto-hide; emits key, vpad, action and tap events. SDL-free. |
| `host/controls/vpad.{h,cpp}` | Snapshot, edge queue, source merge. |
| `host/controls/binding.{h,cpp}` | Mapped-mode table, defaults, overrides, evaluation. SDL-free. |
| `host/controls/gamepad_sdl.cpp` | `SDL_Gamepad` open, close, hot-plug → vpad; rumble out. |
| `host/controls/haptics.{h,cpp}` + `host/sdl/platform_ui_*` | Rumble and tap per platform. |
| `host/controls/raster.{h,cpp}`, `pad_art.{h,cpp}` | Anti-aliased shaded shape primitives, and the DualSense-styled control art drawn with them. |
| `host/controls/overlay.{h,cpp}`, `overlay_view.cpp`, `overlay_paint.{h,cpp}` | `make_view` (SDL-free, unit-tested) resolves what to draw; `Overlay` rasterizes the per-group layers and blends them. Replaces `keypad_overlay`. |
| `host/controls/editor.{h,cpp}`, `editor_actions.{h,cpp}` | Edit mode state, toolbar, picker, snapping; save, reset and the file writes they ask for. |
| `host/controls/layout_store.{h,cpp}`, `controls_host.{h,cpp}` | Layout discovery across the three sources, and the host-side glue that owns the router, the overlay and the settings. |
| `host/keypad_layout.*`, `keypad_modifiers.*` | Scan names and modifier machine kept; half tables move into `builtin_layouts.cpp`. |
| `host/present*.{h,cpp}` | `host_present_set_controls`, `host_present_game_rect`, portrait placement. |
| `host/sdl/main.cpp`, `host/input_gate.cpp` | Wiring; pointer mapping through the game rectangle. |
| `mods/controls_settings.{h,cpp}` | Replaces `keypad_settings`: section 5 settings, migration. |
| `dx/dinput.cpp` | Joystick device (7.3). |
| `dx/xinput.cpp` | XInput DLLs (7.4). |
| `host/Info-ios.plist.in`, `cmake/IosBundle.cmake`, `platform/android/...` | Orientation (8.2). |
| `tools/gen_game_config.py`, `games/stub/game.toml`, `tools/build.py` | `[controls]`, bundling `layouts/`. |
| `host/tests/controls_tests.cpp`, `dx/tests/pad_tests.cpp` | Section 12. `controls_tests` holds every SDL-free `controls::` suite (including the old keypad geometry as a regression oracle); `pad_tests` holds the DirectInput joystick and XInput suites, which need the `host_pad_*` callbacks defined strongly and so cannot live in `dx_tests`. |

## 11. Build order

All six steps land in one PR to `main` (user decision), one or more
commits per step, with every test passing at each commit. CI must be green
on every platform before the PR merges.

1. **Layout model and keypad port.**
   - Includes the JSON model, the router, and the overlay drawing `key`
     controls, plus the tablet `keys` built-in and the settings migration.
   - No visible change: the iPad keypad looks and behaves exactly as
     before.
2. **Pad and mapped binding.**
   - Adds `button`, `dpad`, `stick` and `toggle` controls, the vpad and the
     binding stage.
   - Adds the raster helper, the DualSense look, the tablet `pad` and
     `pad+keys` built-ins, `[controls]` in `game.toml`, layout cycling,
     button haptics and opacity.
   - First target: Majesty on the iPad.
3. **Physical controllers and rumble.**
   - Adds `SDL_Gamepad` on all platforms, auto-hide and
     `pad_with_controller`.
   - Adds rumble to controllers and to the device motor.
4. **Native input.**
   - Adds the DirectInput joystick and the three XInput DLLs.
   - First target: NFSMW. The pin is handed to the NFSMW session.
5. **Phones.**
   - Adds `phone-landscape` and `phone-portrait` built-ins, form-factor
     lookup and the orientation changes.
   - Adds `host_present_game_rect` and the portrait layout.
   - Targets: iPhone and an Android phone.
6. **Editor**, including binding overrides and snapping.

## 12. Testing

- **Unit tests** (all SDL-free):
  - Layout parse, round-trip and error handling, including unknown fields
    and kinds.
  - Form-factor file lookup order.
  - Anchor geometry for several drawable sizes and safe areas, and for
    the portrait controls area.
  - Hit order.
  - Stick dead zone, clamping and the floating recentre.
  - Dpad sectors.
  - Ownership with several fingers.
  - Source merge.
  - Binding hysteresis, cursor speed and override precedence.
  - Settings migration from `host.keypad/*`.
  - Raster primitives against reference pixels.
- **Step-1 regression:** the tablet `keys` built-in yields the same key
  rectangles and scancodes as the old tables at all three sizes.
- **DirectInput tests:** enumeration for DirectInput 7 and 8, `CreateDevice`,
  both data formats, a custom format, range and dead-zone properties,
  buffered data.
- **XInput tests:** packet number changes, user index 1 not connected,
  scaling and Y direction, `XInputEnable`, keystrokes, and rumble calls
  reaching a fake haptics sink.
- **Portrait tests:** `host_present_game_rect` and pointer mapping in
  portrait, i.e. a tap on the game image maps to the right guest
  coordinate.
- **On devices, per step:**
  1. The iPad keypad is unchanged.
  2. Majesty is playable with the pad alone on the iPad.
  3. A DualSense over Bluetooth works on the iPad, Mac and an Android
     device, and rumble works on a phone with no controller.
  4. NFSMW steering and throttle work through the stick and triggers, on
     both the DirectInput and XInput paths if the game offers both.
  5. On an iPhone and an Android phone: landscape and portrait, rotation
     mid-game, and taps landing correctly.
  6. A layout is edited, saved, relaunched and reset on the iPad and in
     phone portrait.

## 13. Risks

- **Game UI hidden under controls in landscape.** The default layouts
  leave the centre free, and opacity is adjustable. Portrait avoids the
  problem entirely.
- **Axis-order expectations differ between games.** The
  `[controls.native]` remap handles this without code.
- **Offering XInput changes which path a game takes.** A game that reads
  both XInput and DirectInput may see the pad twice. It is offered only
  with `pad = "native"`, and a game can set
  `[controls.native] xinput = false` or `dinput = false`.
- **Portrait touches the pointer mapping that every game uses.** All of it
  goes through `host_present_game_rect`, and the step-5 tests cover it.
  Landscape results must stay identical: the rectangle equals today's
  placement whenever the drawable is wider than it is tall.
- **The DualSense look costs CPU raster time.** Uploads happen only on
  state changes, and the stick knob is a separate quad.
- **Editor scope.** The editor is last in the order, so steps 1–5 are
  useful without it, and players can edit the JSON meanwhile.
