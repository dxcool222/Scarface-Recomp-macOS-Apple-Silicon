# M1 iOS design: Populous on a stock iPad

Date: 2026-09-13
Status: approved in conversation 2026-09-13
Parent spec: `2026-09-13-recomp-kit-design.md`, milestone M1

## 1. Goal

Build the existing SDL host for iPadOS and play Populous: The Beginning on
the developer's iPad Pro 11-inch (M4, iPadOS 26.6.1) at the game's native
frame rate, with touch as the primary input, signed automatically with the
developer's Apple team, installed from `tools/build.py`. No JIT, no
jailbreak, no dynamic plugin loading.

## 2. Non-goals

- Mods on iOS. The only Populous mod is a cosmetic display projection and
  the boot only warns when no mods load. Static linking of core mods is later
  work.
- App Store or TestFlight distribution. Owner-built, owner-signed apps only.
- A per-game touch binding file. The mapper ships one generic gesture set;
  `games/<id>/input.toml` waits for a second game.
- Intro movies (`Fmv/`) and the GOG redistributables are not bundled in M1.
- iPhone layout. The device family is iPad; the code does not forbid iPhone
  but nothing is tuned for it.

## 3. Decisions taken in conversation

- Game files are bundled into the app at build time from the developer's
  game directory, then copied into the app's `Documents/game` on first launch
  so the guest's writes have a writable home. This amends the parent spec's
  "never bundled" rule for owner-built iOS apps.
- Touch first; pointer and keyboard accessories work through SDL unchanged
  but are not required for acceptance.
- One build system: an `ios` CMake preset with the Xcode generator and
  automatic signing, driven by `tools/build.py --target ios`.

## 4. What the survey found

Nothing in `runtime/`, `dx/`, the Metal renderer, the audio mixer or the
guest memory model blocks iOS. The changes are at the edges:

| Area | Finding | Action |
|---|---|---|
| `host/sdl/main.cpp` | plain `main`, argv options, file-dialog picker, pointer capture, window sizing, no lifecycle events | platform seam (section 5.2) |
| `host/gpu/metal/metal_surface.mm` | `CGMainDisplayID` refresh-rate query is macOS-only | iOS branch using `UIScreen.maximumFramesPerSecond` |
| `runtime/layout.cpp` | expects `Contents/MacOS` or `dir/resources` | iOS bundles are flat: `resources_dir` is the executable's directory |
| `runtime/kernel32.cpp` | one game root; saves are written into it | seed `Documents/game` from the bundle, point the root there |
| `mods/loader.cpp` | `dlopen` of `.dylib` plugins | no mods on iOS; loader tolerates an empty directory |
| `platform/os_posix.cpp` | `os_temp_dir` falls back to `/tmp`; `os_spawn` is tests-only | honour `TMPDIR`; nothing else |
| CMake, `MacBundle.cmake`, `finish_bundle.py` | macOS bundle layout, ad-hoc codesign, tests everywhere | `if(IOS)` guards, iOS plist, Xcode signing attributes |

## 5. Architecture

### 5.1 CMake `ios` preset

- `CMakePresets.json` gains configure preset `ios`: generator `Xcode`,
  `CMAKE_SYSTEM_NAME=iOS`, `CMAKE_OSX_DEPLOYMENT_TARGET=17.0`,
  `CMAKE_OSX_ARCHITECTURES=arm64`, binary dir `build/cmake/ios`,
  `POP_TRANSLATE=ON` (the real translation is required), plus a build preset
  of the same name. A `ios-stub` variant with `POP_TRANSLATE=STUB` exists for
  CI compile checks.
- Top-level `CMakeLists.txt`: `POP_BUILD_DIR` is `build/ios` for iOS so the
  app never collides with the macOS bundle. `RECOMP_IOS_TEAM` is a cache
  string defaulting to `$ENV{RECOMP_IOS_TEAM}`; iOS configure fails with a
  clear message when it is empty.
- Under `if(IOS)`: no `add_test`, no test executables, no `pop_headless`,
  `pop_smoke`, `pop_fixture`, `profile_tests`, `present_events_tests`, no
  plugin targets (`Plugins.cmake` functions become no-ops), no `core_mods`
  dependency. `recomp_app` and the libraries it links are the whole graph.
- `cmake/IosBundle.cmake` (new) configures `host/Info-ios.plist.in` and sets
  on `recomp_app`: `MACOSX_BUNDLE ON`, `MACOSX_BUNDLE_INFO_PLIST`,
  `XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER=${RECOMP_BUNDLE_ID}`,
  `XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=${RECOMP_IOS_TEAM}`,
  `XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic`,
  `XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY=2`,
  `XCODE_ATTRIBUTE_INFOPLIST_KEY_UISupportedInterfaceOrientations~ipad`
  landscape only, and a post-build step that runs
  `tools/stage_game_files.py` to copy the filtered game directory into the
  bundle at `game/`.
- `host/Info-ios.plist.in`: `CFBundleIdentifier`, names and version from the
  game config; `LSRequiresIPhoneOS`, `UIDeviceFamily [2]`,
  `UIRequiresFullScreen`, `UIStatusBarHidden`, `UILaunchScreen` (empty
  dictionary), landscape orientations, `UIFileSharingEnabled` and
  `LSSupportsOpeningDocumentsInPlace` so saves and logs are reachable.
- SDL3 keeps coming from `FetchContent` and builds for iOS unchanged.

### 5.2 Platform seam in the SDL host

New header `host/sdl/platform_ui.h` with two implementations chosen by
CMake: `platform_ui_desktop.cpp` (today's behaviour, extracted) and
`platform_ui_ios.mm`.

```c++
void platform_ui_init_hints();
GamePath platform_ui_resolve_game(const char *flag, std::string *error);
SDL_Window *platform_ui_create_window(const char *title, int mode_w, int mode_h, int scale,
                                      SDL_WindowFlags surface_flag, int *window_mode);
bool platform_ui_handle_lifecycle(const SDL_Event &e);
```

Pointer capture needs no seam: `SDL_HideCursor`, `SDL_ShowCursor` and
`SDL_SetWindowMouseRect` are harmless no-ops on iOS.

`main.cpp` includes `SDL3/SDL_main.h` (a no-op on desktop, the UIKit entry
on iOS), calls these four functions where it used to do the work inline, and
routes finger events to the touch mapper. Options that came from argv on
desktop (`--exe`, `--version`, `--probe-layout`) stay desktop-only; the iOS
build has no argv.

### 5.3 Game files

- `tools/stage_game_files.py --game-dir games/populous --source original/gog --dest <bundle>/game`
  copies the game directory excluding `__redist`, `commonappdata`, `app`,
  `Fmv`, `*.dll`, `*.cmd`, `dxcfg.*` and writes `game/.stamp` containing the
  executable's SHA-256. The exclusion list lives in `games/populous/game.toml`
  under `[bundle].exclude` so another game can differ.
- First launch on iOS: `platform_ui_ios::resolve_game_exe` compares
  `<bundle>/game/.stamp` with `Documents/game/.stamp`; when absent or
  different it removes `Documents/game`, copies the bundle's `game/` there,
  writes the stamp, and returns `Documents/game/<executable>`. The loader then
  verifies the hash exactly as on desktop.
- `runtime/kernel32.cpp` needs no change: `g_game_dir` is derived from the
  executable path the host hands the loader, and it already maps the guest
  root onto that directory case-insensitively.

### 5.4 Touch mapper

`host/input_touch.h/.cpp`: a state machine over SDL finger events that emits
synthesized `SDL_Event`s (mouse motion, mouse buttons, key presses) into the
same queue `input.cpp` already consumes, so nothing downstream learns about
touch. Coordinates are normalised finger positions scaled to the window's
pixel size.

| Gesture | Emits |
|---|---|
| Tap (release within 350 ms, travel under 20 px) | motion to point, left down, left up 90 ms later |
| Long press (rested 350 ms, travel under 20 px) then lift | motion to point at 350 ms; right down, right up on the lift. Populous: unselect followers or a spell, query an object |
| Long press then drag | wheel-button down at the press point, relative motion while moving, wheel-button up at release. Populous scrolls the map with a wheel-button drag, and rotates when it starts near the bottom of the screen |
| Hold within 16 pt of a window edge | the cursor is placed exactly on that edge for as long as the finger rests (edge scrolling); no right click; on the lift the cursor moves 48 pt inside so scrolling stops |
| One-finger drag (travel over 20 px) | left down at start, motion while moving, left up at release |
| Two-finger drag | arrow-key down/up pulses matching the dominant direction, one pulse per 24 px |
| Two-finger tap | motion to the point between the two fingers, right down, right up |
| Three-finger tap | F10 down, F10 up (Options) |
| Four-finger tap | toggles the on-screen keyboard (`SDL_StartTextInput` / `SDL_StopTextInput`) |

A key bar (`host/touch_overlay.cpp`, layout in `touch_overlay_layout.h`;
to be replaced by the split keypad of `2026-09-13-keypad-design.md`) is
drawn by the presenter along the bottom edge when no hardware keyboard is
attached: Esc, F10, the four arrows, Space, Enter, and HIDE, which collapses
the strip to a KEYS tab in the bottom-right corner. A finger on a key holds
that key until it lifts and never reaches the gesture mapper. Thresholds are
constants in `input_touch.h`.

Two facts about the game shaped the click path, both measured on the device:

- Populous hit-tests against the cursor it integrates from relative motion
  and the host's damped correction can take hundreds of milliseconds to walk
  it across the screen. A touch therefore writes the game's cursor pair
  directly (`host_gate_pointer_place`, offsets 0x20/0x24 of the mouse device
  object) through an ordered `PLACE` input between the motion and the click.
- The game samples its mouse buttons once per frame, so a press and release
  inside one frame is invisible. A synthesized click stays pressed for
  `kTouchClickHoldNs` (90 ms) and releases from the mapper's tick.

### 5.5 Metal and layout

- `metal_surface.mm`: under `TARGET_OS_IPHONE`, the refresh rate comes from
  `UIScreen.mainScreen.maximumFramesPerSecond`; `SDL_Metal_CreateView` and
  the `CAMetalLayer` path are already portable.
- `runtime/layout.cpp`: when the executable's directory has no
  `Contents/MacOS` suffix and no `resources/` child but contains `Info.plist`
  (a flat bundle), `resources_dir` is that directory. `profile_dir` stays
  `os_user_data_dir(RECOMP_APP_NAME)`, which resolves inside the container.
- `platform/os_posix.cpp`: `os_temp_dir` returns `$TMPDIR` when set before
  falling back to `/tmp`.

### 5.6 Build script and device tooling

`tools/build.py` gains `--target ios` (macOS host only) with `--device <udid>`,
`--team <id>` and `--no-install`:

1. Require `build/recomp/gen/table.c`; tell the user to run `--regenerate`
   on macOS first otherwise.
2. `cmake --preset ios -DRECOMP_GAME=<game> -DRECOMP_IOS_TEAM=<team>`.
3. `cmake --build --preset ios --target recomp_app -- -allowProvisioningUpdates`.
4. Unless `--no-install`: pick the device (`xcrun devicectl list devices
   --json-output`, the one available iPad, else require `--device`), then
   `xcrun devicectl device install app --device <udid> build/ios/<App>.app`
   and `xcrun devicectl device process launch --terminate-existing --device
   <udid> <bundle id>`.

`tools/ios_logs.py` pulls the app's `Documents` and the gameplay log with
`devicectl device copy from --domain-type appDataContainer`.

SDL3 delivers `SDL_EVENT_WILL_ENTER_BACKGROUND` and its siblings only to
event watchers (`SDL_AddEventWatch`), never through the queue; the iOS seam
registers a watcher, and `host_present_suspend(true)` drains in-flight GPU
work because the GPU completes nothing in the background.

## 6. Error handling

| Condition | Behaviour |
|---|---|
| `RECOMP_IOS_TEAM` unset | configure fails: "set RECOMP_IOS_TEAM or pass --team; `security find-identity -v -p codesigning` lists teams" |
| No generated translation | `build.py` stops before configuring with the regenerate hint |
| Two or more devices available, no `--device` | `build.py` lists them and stops |
| Bundle has no `game/` or the hash mismatches | full-screen message with expected executable and hash, same text in the log, exit |
| First-launch copy fails | partial `Documents/game` removed, message and log as above |
| Backgrounded during play | audio stream paused, presentation stopped, guest paused at the next tick; resumed on foreground |
| Memory pressure | nothing new: the 256 MB arena is lazily faulted; the game's own save is the recovery if iOS terminates the app |

## 7. Testing

- macOS unit tests: `host/tests/input_touch_tests.cpp` covers tap, long
  press, drag, two-finger drag pulses, two-finger tap, cancellation on a
  second finger, and coordinate scaling. `runtime/tests/runtime_tests.cpp`
  gains a layout case for the flat-bundle branch via `POP_LAYOUT_EXE`
  injection. Both run under the existing `nogame` label.
- Compile checks: `tools/test.py` keeps passing; CI gains an `ios-stub`
  configure and build of `recomp_app` on the macOS runner (no signing:
  `CODE_SIGNING_ALLOWED=NO`).
- Device: `tools/build.py --target ios` installs and launches; `tools/ios_logs.py`
  pulls the log. Acceptance is by hand on the iPad.

## 8. Acceptance

On the iPad Pro, launched from the home screen after `tools/build.py --target ios`:

1. The main menu appears fullscreen in landscape.
2. A level loads from the menu by touch alone.
3. Units can be selected and ordered by tap and drag; the camera pans with a
   two-finger drag; a right click (two-finger tap) and Options (three-finger tap) work.
4. The pulled gameplay log shows the game's native frame rate sustained
   during play (the same measure the macOS smoke run reports).
5. Backgrounding and returning resumes play with audio.

## 9. Risks

- SDL3's iOS main-thread rules: the host already pumps events from the
  boot tick on the main thread as macOS requires, but a guest worker
  reaching the pump on iOS would fault; `boot_on_run_thread()` guards it.
- Compile time: the generated code compiles once more for arm64 iOS,
  roughly the macOS build time; incremental afterwards.
- Populous's UI was designed for a precise pointer at 640x480; long-press
  right click and small hit targets may need threshold tuning during
  acceptance. Thresholds are constants for that reason.
- Automatic signing needs Xcode to have the Apple ID for the team signed in
  once; `-allowProvisioningUpdates` handles profile creation after that.
