# Building twiNX for Nintendo Switch

## Supported playback stack

twiNX 0.9.0 is validated with this exact Switch playback stack:

```text
MPV:    switch-libmpv 0.36.0-7
FFmpeg: switch-ffmpeg 7.1-12
```

The repository recipes include twiNX guards for MPV frame side data and for
FFmpeg/NVTEGRA H.264 references, decode maps and hardware-frame transfers.
Using a different MPV or FFmpeg package may compile, but it is not the playback
stack validated against Twitch stream discontinuities.

## Requirements

Install a current devkitPro Nintendo Switch environment with `devkitA64`,
`libnx`, CMake, Ninja, pkg-config, curl and WebP support. The MPV recipe also
requires Python and Meson. Borealis and LunaSVG source are included in the
repository.

Run package-building commands from the devkitPro MSYS2 shell. Run the final
Windows application build from Command Prompt or PowerShell.

## 1. Build and install the supported MPV

In devkitPro MSYS2:

```bash
cd scripts/switch/mpv
rm -rf src pkg build
makepkg -Csf --noconfirm
pacman -U --noconfirm switch-libmpv-0.36.0-7-any.pkg.tar.zst
pacman -Q switch-libmpv
```

The final command must report:

```text
switch-libmpv 0.36.0-7
```

## 2. Build the guarded FFmpeg package

Still in devkitPro MSYS2:

```bash
cd scripts/switch/ffmpeg-libnx
rm -rf src pkg
makepkg -Csf --noconfirm
```

Expected package:

```text
switch-ffmpeg-7.1-12-any.pkg.tar.zst
```

Extract it into the repository-local dependency root:

```bash
cd ../../..
rm -rf .twinx-build/ffmpeg-local-root
mkdir -p .twinx-build/ffmpeg-local-root
bsdtar -xf scripts/switch/ffmpeg-libnx/switch-ffmpeg-7.1-12-any.pkg.tar.zst \
  -C .twinx-build/ffmpeg-local-root
```

The `.twinx-build` directory is ignored by Git.

## 3. Build twiNX

### Windows PowerShell

```powershell
.\build-switch.ps1
```

To use an FFmpeg package extracted elsewhere:

```powershell
.\build-switch.ps1 -FfmpegRoot "C:\path\to\switch-portlibs"
```

### Linux or another POSIX shell

```bash
./build-switch.sh
```

Or with a custom dependency root:

```bash
TWINX_FFMPEG_ROOT=/path/to/switch-portlibs ./build-switch.sh
```

## Output

```text
build_switch/TwiNX.nro
build_switch/TwiNX.elf
```

Keep each NRO together with its matching ELF. Atmosphere crash addresses can
only be symbolicated accurately with the ELF from the exact same build.

## Dependency warning

When no custom FFmpeg root is available, CMake uses the FFmpeg libraries
reported by the installed `mpv.pc`. The project may compile, but that build does
not necessarily include the TLS and NVTEGRA guards validated for twiNX 0.9.0.
