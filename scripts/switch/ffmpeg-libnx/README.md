# Nintendo Switch FFmpeg 7.1 libnx TLS recipe

This directory contains the reproducible `switch-ffmpeg` package recipe used
for the twiNX 0.9.0 release build. It adds Nintendo Switch/libnx TLS support so
FFmpeg can open HTTPS Twitch playlists directly, plus the validated twiNX
H.264-reference and NVTEGRA hardware-frame transfer guards.

The recipe downloads the official FFmpeg 7.1 source archive and verifies its
SHA-256 hash. Generated source trees, packages and static libraries are not
committed to the repository.

## Build in the devkitPro MSYS2 shell

From this directory:

```bash
makepkg -Csf --noconfirm
```

Extract the generated package into twiNX's local dependency root:

```bash
mkdir -p ../../../.twinx-build/ffmpeg-local-root
bsdtar -xf switch-ffmpeg-7.1-12-any.pkg.tar.zst \
  -C ../../../.twinx-build/ffmpeg-local-root
```

The normal twiNX build scripts automatically detect that local root. A custom
location can instead be supplied through `TWINX_FFMPEG_ROOT`; it must point to
the directory containing `include/` and `lib/`.
