<div align="center">

# Scarface Recomp

### Native Apple Silicon Port

**Scarface: The World Is Yours (2006)**  
*Static recompilation of the original Windows PC version for macOS ARM64*

![ARM64](https://img.shields.io/badge/ARCH-ARM64-8A2BE2?style=for-the-badge)
![macOS](https://img.shields.io/badge/PLATFORM-macOS-111111?style=for-the-badge&logo=apple&logoColor=white)
![Experimental](https://img.shields.io/badge/STATUS-EXPERIMENTAL-C46A00?style=for-the-badge)
![Target](https://img.shields.io/badge/TARGET-Scarface%20v1.00.2-2455A4?style=for-the-badge)
![Language](https://img.shields.io/badge/LANGUAGE-C%20%7C%20C%2B%2B-00599C?style=for-the-badge)

<br>

A fan-made native Apple Silicon port built through static recompilation.

</div>

---

<div align="center">

> **THE WORLD IS YOURS**
>
> An unofficial native Apple Silicon port of *Scarface: The World Is Yours*.

</div>

> **Bring your own game files.** This project does not include `scarface.exe`, the game's archives, movies, or other retail assets. You need your own original PC game files from **Scarface v1.00.2**, accessible from your Mac.

## About this project

This is my **first attempt at a full Windows x86-to-ARM64 static recompilation**. While I have worked on game ports before, this project has been an entirely new learning experience for me.

This is a passion project focused on learning reverse engineering, static recompilation, compatibility layers, native rendering, audio systems, and the challenges involved in bringing older Windows software to modern Apple hardware.

It is an **experimental work in progress**, not a polished commercial release. Gameplay works in development builds, but audio, graphics, compatibility, and performance may still need improvement.

**The generated C code is included in this repository.** You do **not** need IDA, Ghidra, or the original recompilation toolchain just to build the Mac app.

## Requirements

You will need:

- An Apple Silicon Mac with an M-series chip
- Your own legal PC copy of *Scarface: The World Is Yours*
- The complete original PC game installation and data files, accessible from your Mac
- A matching `v1.00.2` `scarface.exe`
- Xcode Command Line Tools
- Homebrew
- CMake
- Ninja
- Python 3.12
- `libvorbis`, `libogg`, and OpenAL Soft
- An internet connection for the initial build dependencies

> Intel Macs are not supported by this project.

You do **not** need to install the Windows game through a Windows installer on macOS. The original PC installation files can be copied from a Windows installation, stored on an external drive, or placed in any folder your Mac can access.

## Verify your `scarface.exe`

The generated game code targets **one specific PC executable**. A different patch, edition, modified executable, or release may not work, even if Windows labels it `v1.00.2`.

Open Terminal and run the following command, replacing the example path with the location of your executable:

```bash
shasum -a 256 "/path/to/your/scarface.exe"
```

The required SHA-256 is:

```text
aee792ee4552e28c533abab7992dd5ad542e8ff12806cbcedab300842d86f6f5
```

If the hash matches, your executable is the version targeted by this build.

If it does not match, that does **not** necessarily mean your copy is illegitimate. It only means this release has not been verified against that executable, and it may not work correctly.

> The EXE alone is not enough. You need the complete original game installation because the runtime reads the game data alongside `scarface.exe`.

## Build the Mac app

### 1. Download the project

On GitHub, select **Code → Download ZIP**, extract the archive, and open Terminal.

Move into the extracted project directory:

```bash
cd /path/to/Scarface-Recomp-macOS-Apple-Silicon
```

You can also type `cd ` with a space after it, drag the extracted project folder from Finder into Terminal, and press **Return**.

### 2. Install the build tools

Install [Homebrew](https://brew.sh) if it is not already installed. Then run:

```bash
xcode-select --install
brew install cmake ninja python@3.12 libvorbis libogg openal-soft
```

If the Xcode Command Line Tools are already installed, macOS may simply tell you so.

### 3. Compile

From inside the project directory, run:

```bash
python3.12 tools/build.py --target app
```

The first build may take a while. It downloads and builds additional dependencies, including components such as SDL, glslang, and FFmpeg.

When the build finishes, the application should be here:

```text
build/ScarfaceRecomp.app
```

> You do **not** need `scarface.exe` to compile. You only need your matching game installation when launching the app.

## Play the game

From the project directory, run:

```bash
./build/ScarfaceRecomp.app/Contents/MacOS/ScarfaceRecomp --exe "/path/to/your/scarface.exe"
```

Replace the example path with the actual location of your executable. Keep the quotation marks if the path contains spaces.

You can type the command up to `--exe ` and drag `scarface.exe` from Finder into Terminal to insert its path automatically.

The original game installation does **not** need to be inside this repository. It can be stored anywhere accessible from your Mac, including another drive, as long as the executable can access its accompanying game data.

## Project status

This project is an **experimental work in progress**. The current build is not intended to be considered a finished or fully compatible commercial release.

The port is being developed and tested on Apple Silicon Macs, but behavior may still differ from the original Windows release.

## Known limitations

Audio, graphics, loading behavior, compatibility, and performance may still need improvement. Possible issues include:

- Audio glitches, interruptions, or timing differences
- Graphical bugs or missing effects
- Performance drops in certain scenes
- Compatibility problems with unsupported executable versions
- Differences from the original Windows release
- Features that have not yet been fully tested

When reporting a problem, include clear reproduction steps, terminal output, your Mac model, your macOS version, and the SHA-256 result of your executable if possible.

## Project structure

- `gen/` — generated C code translated from the targeted retail executable
- `kit/` — native runtime, compatibility code, and supporting systems
- `tools/` — build and project tooling
- `LICENSE` — project licensing information
- `NOTICE` — third-party notices and attribution

## Credits and licenses

See the following files for project and third-party licensing information:

- [`LICENSE`](LICENSE)
- [`NOTICE`](NOTICE)
- [`kit/LICENSE`](kit/LICENSE)
- [`kit/NOTICE`](kit/NOTICE)

These files do not grant rights to the original game, its assets, or its retail data. No retail game files are included in this repository.

This is an unofficial fan project and is **not affiliated with, endorsed by, sponsored by, or connected to** the game's publishers or rights holders.

## Final note

This project started as a personal challenge: to see whether a classic Windows x86 game could be brought to life natively on Apple Silicon.

It is my first full Windows x86-to-ARM64 static recompilation, and there is still a lot to learn and improve. Thanks for checking it out, testing it, reporting issues, and following the project.
