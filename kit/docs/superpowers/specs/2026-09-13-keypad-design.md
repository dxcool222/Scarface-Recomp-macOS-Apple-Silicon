# Keypad design: a split on-screen keyboard for touch play

Date: 2026-09-13
Status: **superseded** by `2026-09-17-touch-controls-design.md`, which
replaced the keypad with the data-driven controls system. Read that
document for how the on-screen keyboard works now; this one is kept for the
reasoning behind the split-halves layout, the modifier machine and the key
tables, all of which the `keys` built-in layout still reproduces.
Previously: implemented 2026-09-13; accepted on the iPad (layout, tabs,
hold/latch/lock, F10 size, relaunch persistence)
Parent spec: `2026-09-13-recomp-kit-design.md`; replaces the key strip of
`2026-09-13-m1-ios-design.md` section 5.4

## 1. Goal

Give every game the kit builds one on-screen keyboard that reaches almost
every key a PC game of the era reads, including Shift, Ctrl and Alt, and
that two thumbs can use while the game stays visible: a compact keyboard
split into two halves parked in the bottom corners of a landscape tablet.
The Populous-specific eight-key strip goes away.

## 2. Decisions taken in conversation

- Key set: a full compact keyboard only. No per-game key list; a game's
  `game.toml` may choose the default visibility and nothing else.
- Layout: split keyboard, one half per bottom corner. The left half holds
  the left side of a QWERTY board with Esc, Tab, Shift, Ctrl and Alt; the
  right half the right side with Backspace, Enter, Space and the arrows.
  Function keys sit in a row above each half.
- Key size: 36 pt at the default step, with 32 pt and 40 pt steps.
- Modifiers: hold to chord, tap to latch for the next key, double tap to
  lock until tapped again.

## 3. Non-goals

- Text entry. The four-finger toggle of the system keyboard stays for
  typing names; the keypad is for game keys.
- Key repeat. A held key stays down; games that repeat do so from their own
  polling of key state, as with a physical keyboard.
- A visual theme system. One look, drawn by the same raster-and-blend path
  as the strip it replaces.
- iPhone layouts and portrait orientation.

## 4. Layout

Each half is a grid of 8 columns by 5 rows of cells. A cell is the key size
plus a 4 pt gap: 36, 40 or 44 pt of pitch for the three size steps, so a
half is 320 by 200 pt at the default step. On an 11-inch iPad (1180 pt
wide) the two halves leave 540 pt of game between them; the game also stays
visible above them. `span` is a key's width in cells.

Left half, bottom-left corner, rows top to bottom:

| Row | Keys |
|---|---|
| 0 | Esc, F1, F2, F3, F4, F5, F6, Ins |
| 1 | `` ` ``, 1, 2, 3, 4, 5, 6, Home |
| 2 | Tab, Q, W, E, R, T, [, PgUp |
| 3 | Shift, A, S, D, F, G, ], PgDn |
| 4 | Ctrl, Alt, Z, X, C, V, B, \ |

Right half, bottom-right corner:

| Row | Keys |
|---|---|
| 0 | F7, F8, F9, F10, F11, F12, Del, End |
| 1 | 7, 8, 9, 0, -, =, Backspace (span 2) |
| 2 | Y, U, I, O, P, ;, ', Enter |
| 3 | H, J, K, L, `,`, ., /, Up |
| 4 | Space (span 3), N, M, Left, Down, Right |

The three modifiers are on the left half so the left thumb chords while the
right thumb taps letters, and latching covers chords within one half.

Each half has a tab, 64 by 20 pt, above its outer bottom corner: HIDE while
the half is shown, KEYS alone in the corner while it is hidden. Tapping the
tab toggles that half. A hardware keyboard attached hides both halves and
their tabs, as the strip does today.

Labels are drawn with the host's 6x8 font at twice its size; the longest
label (Shift, Space, Enter) is five glyphs, 60 px, inside the smallest key
at the smallest step on a 2x display (64 px).

## 5. Behaviour

- A finger that lands on a key holds that key's scancode down until it
  lifts or is cancelled; it never reaches the gesture mapper. Motion is
  ignored, so a finger that slides off a key keeps holding it.
- A finger that lands on a tab toggles the half on lift-free press, as the
  strip's HIDE does.
- Fingers that land anywhere else are game gestures (`TouchMapper`),
  including the area between the halves and above them.
- Modifiers (Shift, Ctrl, Alt): a state machine per modifier with states
  Off, Held, Latched and Locked.
  - Off → Held on finger down; Held → Off on lift after 250 ms or more.
  - Held → Latched on a lift under 250 ms (a tap). Latched → Off when the
    next non-modifier key lifts. A second tap within 400 ms of the first →
    Locked. Locked → Off on the next tap.
  - Key down is emitted on leaving Off; key up on entering Off. Games
    therefore see the modifier down for the whole latched or locked span.
  - A latched or locked modifier is drawn lit.
- The tabs, sizes and visibility persist: `host.keypad/left` (0 or 1),
  `host.keypad/right` (0 or 1) and `host.keypad/size` (0, 1, 2) through
  the mod settings store, declared like the display rows so they appear on
  the F10 settings page (Keypad size: Small, Medium, Large). Modifier state
  is not persisted.
- `games/<id>/game.toml` gains `[touch] keypad = "auto" | "hidden"`. `auto`
  (the default) shows both halves when no hardware keyboard is attached and
  no saved setting exists; `hidden` starts with both halves hidden. This
  is the only per-game knob.

## 6. Components

| File | Responsibility |
|---|---|
| `host/keypad_layout.h/.cpp` | SDL-free tables and geometry: the two key tables (label, scancode as an integer, column, row, span), cell pitch per size step, the rectangle of each half, tab and key in drawable pixels for a drawable size and scale, and the hit test returning None, Key (side, scancode) or Toggle (side). |
| `host/keypad_modifiers.h/.cpp` | The modifier state machine above; consumes key down/up with a clock and emits the key down/up actions to send, plus `lit(scancode)` for drawing. SDL-free. |
| `host/keypad_overlay.h/.cpp` | Rasterizes each half and tab at drawable size, uploads once per size or state change, blends over the frame in the presenter. Replaces `touch_overlay.h/.cpp`. |
| `host/present.h`, `present_thread.cpp` | `host_present_set_keypad(KeypadView)` replaces `host_present_set_touch_overlay(int)`; the view carries per-half visibility, size step, lit modifiers and drawable size. |
| `host/sdl/main.cpp` | Routes finger down to the keypad hit test before the gesture mapper, holds keys per finger, feeds the modifier machine, toggles halves, saves settings. |
| `mods/keypad_settings.h/.cpp` | Declares and reads the three settings the way `display_settings.cpp` does for display rows. |
| `tools/gen_game_config.py`, `games/populous/game.toml` | `RECOMP_TOUCH_KEYPAD_DEFAULT` from `[touch] keypad`. |
| `host/tests/keypad_tests.cpp` | Layout, hit test and modifier tests; the touch overlay tests in `input_touch_tests.cpp` are removed with the strip. |

Removed: `host/touch_overlay_layout.h/.cpp`, `host/touch_overlay.h/.cpp`.

## 7. Data flow

Finger down → `keypad_hit(view, drawable, px, py)`:

- Key: record finger → scancode; if the scancode is a modifier, feed
  `KeypadModifiers::press`; else push SDL key down. On lift or cancel: feed
  `release` or push key up; the modifier machine's emitted actions are
  pushed as SDL key events in order. A non-modifier key's lift also tells
  the machine so a latched modifier releases.
- Toggle: flip the half, save the setting, publish the view.
- None: `TouchMapper::finger_down` as today.

The presenter draws the two halves and tabs from the last published view,
after the game frame and before the performance overlay.

## 8. Error handling

- A drawable too small for both halves (under 700 pt wide at the chosen
  size) draws them anyway, overlapping in the middle; nothing else is
  possible on a landscape tablet and the setting steps down are available.
- A finger cancelled by the system releases its key; a modifier cancelled
  while Held goes to Off, never to Latched.
- Focus loss and backgrounding release every held key and every modifier
  (`cancel_all`), as the strip does.
- Settings that fail to load fall back to the `game.toml` default; a save
  failure is logged once and play continues.

## 9. Testing

- `keypad_tests` (native, `nogame`): every key's centre hit-tests to its
  own scancode at all three sizes on 1180x820 at scale 2 and on 1024x768 at
  scale 1; no key rectangle of one half overlaps the other half; the tabs
  sit at the corners and toggle; a hidden half hit-tests only its tab; the
  modifier machine's Held, Latched, Locked and cancel transitions emit
  exactly the key events listed in section 5.
- `input_touch_tests` keep the gesture cases; the overlay cases move.
- On the iPad: hold Shift and tap a letter (chord), tap Shift then a letter
  (latch), double tap Shift then two letters (lock), hide and show each
  half, change the size on the F10 page, relaunch and confirm the halves
  and size come back.

## 10. Migration

The strip's `HIDE`/`KEYS` state is not carried over. The Populous play
notes in `README.md` and the M1 spec's section 5.4 point at this document.
