# Building twiNX for Nintendo Switch

## Requirements

Install a current devkitPro Nintendo Switch development environment with
`devkitA64`, `libnx`, CMake, Ninja, pkg-config, libmpv, curl and WebP support.
The project also vendors Borealis and LunaSVG source code.

The exact 0.8.1 release used the GPL-enabled FFmpeg 7.1 libnx TLS recipe in
`scripts/switch/ffmpeg-libnx`. This recipe is recommended because Twitch media
playlists use HTTPS.

## 1. Prepare the custom FFmpeg package

Open the devkitPro MSYS2 shell and run:

```bash
cd scripts/switch/ffmpeg-libnx
makepkg -s --noconfirm
mkdir -p ../../../.twinx-build/ffmpeg-local-root
bsdtar -xf switch-ffmpeg-7.1-5-any.pkg.tar.zst \
  -C ../../../.twinx-build/ffmpeg-local-root
cd ../../..
```

The local dependency directory is ignored by Git.

## 2. Build twiNX

### Windows PowerShell

```powershell
.\build-switch.ps1
```

### Linux or a POSIX shell

```bash
./build-switch.sh
```

Output:

```text
build_switch/TwiNX.nro
```

## Custom FFmpeg location

Set `TWINX_FFMPEG_ROOT` to a directory containing `include/` and `lib/` when the
custom package is stored elsewhere.

PowerShell example:

```powershell
.\build-switch.ps1 -FfmpegRoot "C:\path\to\switch-portlibs"
```

POSIX example:

```bash
TWINX_FFMPEG_ROOT=/path/to/switch-portlibs ./build-switch.sh
```

When no override is available, CMake uses the FFmpeg libraries reported by the
installed `mpv.pc`. The project may compile, but HTTPS Twitch playback depends
on the capabilities of that installed FFmpeg build.
