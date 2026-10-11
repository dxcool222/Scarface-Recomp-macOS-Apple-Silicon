# Scarface Recomp — macOS Apple Silicon

An **unofficial native Apple Silicon port** of **Scarface: The World Is Yours** (2006), built using static recompilation of the PC version.

> **Bring your own game.** This project does **not** include `scarface.exe`, the game's archives, movies, or other retail assets. You need your own PC installation of **Scarface v1.00.2** to play.

## About this project

This is **my first time doing anything like this**: taking a Windows x86 game and getting it running natively on an Apple Silicon Mac through static recompilation. I've learned a lot while working on it, and I'm sharing the results for other people who love this game and want to experiment with it.

It's a **work in progress**, not a polished commercial release. Gameplay works in the development build, but there may still be bugs, imperfect audio or graphics, and performance drops. Thanks for being patient with a first-time project of this kind!

**This repository includes the generated C code and the modified native runtime.** You do **not** need IDA, Ghidra, or the original recompilation toolchain just to build the Mac app.

## What you need

- An **Apple Silicon Mac** (M-series chip; **not** an Intel Mac).
- Your **own PC copy** of *Scarface: The World Is Yours*, including its game data files.
- The **matching v1.00.2 `scarface.exe`** described below.
- An internet connection for the initial build dependencies.

### Important: check your `scarface.exe` first

The generated game code in this repository comes from **one specific PC executable**. A different patch, edition, or modified EXE may not work, even if it is labeled v1.00.2.

Open **Terminal** and run this, replacing the example path with your own:

```bash
shasum -a 256 "/path/to/your/scarface.exe"
```

**Required SHA-256:**

```text
aee792ee4552e28c533abab7992dd5ad542e8ff12806cbcedab300842d86f6f5
```

If your result matches, you have the executable this build targets. **If it doesn't match, that doesn't mean your game is illegitimate**—only that this release has not been verified for that executable. Keep your original game files intact; don't rename or modify an EXE just to make its hash look correct.

**You need the whole game installation to play, not just the EXE.** The runtime reads the game data alongside `scarface.exe`. Your installation can be anywhere on your Mac.

## Build the Mac app

### 1. Get the project

On GitHub, click **Code → Download ZIP**, extract it, and open **Terminal**.

Type `cd ` (with a space after it), then drag the extracted project folder from Finder into Terminal and press **Return**. This puts Terminal in the correct folder, whatever the folder is named.

### 2. Install the build tools

Install [Homebrew](https://brew.sh) if you don't have it already. Then run:

```bash
xcode-select --install
brew install cmake ninja python@3.12 libvorbis libogg openal-soft
```

If the Xcode Command Line Tools are already installed, macOS may simply tell you so.

### 3. Compile

From inside the project folder, run:

```bash
python3.12 tools/build.py --target app
```

The first build can take a while. It downloads and builds dependencies such as SDL, glslang, and FFmpeg. A clean Apple Silicon build of this release has been completed successfully, but build time will vary by Mac.

When it's done, the app is here:

```text
build/ScarfaceRecomp.app
```

**You do not need `scarface.exe` to compile.** You only need your matching game installation when you run the app.

## Play

In Terminal, while still inside the project folder, run:

```bash
./build/ScarfaceRecomp.app/Contents/MacOS/ScarfaceRecomp --exe "/path/to/your/scarface.exe"
```

Replace `/path/to/your/scarface.exe` with the actual location of your copy. Keep the quotation marks when your path has spaces.

**Tip:** On macOS, you can type the command up through `--exe ` and drag `scarface.exe` from Finder into Terminal to fill in its path.

The game does **not** have to live inside this repository. You can keep it on another drive or in any folder accessible to your Mac.

## Troubleshooting

**`python3.12: command not found`** — Make sure the Homebrew install command completed. Open a new Terminal window and try again.

**Build fails while downloading dependencies** — Check your internet connection, then retry the build command. If the failure repeats, include the full error when reporting it.

**The EXE hash doesn't match** — This build targets the exact SHA-256 above. Other executable versions haven't been verified, even if their version labels look similar.

**The app builds but the game doesn't load correctly** — Check that the `--exe` path is correct and that the original game data is in the same installation folder. An EXE alone isn't enough.

**Sound glitches, graphical bugs, or FPS drops** — This is still an experimental port. Some issues remain under investigation; reporting a reproducible problem is helpful.

## Notes and credits

- **Platform:** macOS on Apple Silicon. This repository is **not** a Nintendo Switch build.
- **Approach:** Native static recompilation of the original PC x86 executable, with a modified compatibility/runtime layer and Metal rendering.
- **Game files:** Not included. You must supply your own PC installation.
- **Generated code:** `gen/` contains translated code derived from the retail executable; `kit/` contains the native runtime and supporting code.
- **Licenses and notices:** See `LICENSE`, `NOTICE`, `kit/LICENSE`, and `kit/NOTICE` for the applicable project and third-party terms. Those files do not grant rights to the original game.

This is an unofficial fan project and is **not affiliated with or endorsed by** the game's publishers or rights holders.
