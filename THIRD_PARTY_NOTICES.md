# Third-party notices

This file summarizes the principal upstream projects used by twiNX. Copyright
headers and license files inside third-party source directories remain
authoritative.

## Switchfin 0.9.1

- Upstream: `dragonflylee/switchfin`, tag `0.9.1`
- Role: original application, Nintendo Switch, Borealis and mpv foundation
- License: Apache License 2.0

A copy of Apache-2.0 is stored at `LICENSES/Apache-2.0.txt`. Existing upstream
copyright and license headers have been retained.

## Borealis

- Upstream: `dragonflylee/borealis`
- Vendored commit: `7f03684805befffb1be18a1b2bdb81fa66945af2`
- Role: controller-first user-interface framework
- License: Apache License 2.0

The upstream license and NOTICE remain in `library/borealis/`; the NOTICE is
also copied to `LICENSES/NOTICE-Borealis.txt`.

## LunaSVG

- Upstream: `sammycage/lunasvg`
- Vendored commit: `f924651b85cac47dbe15f51a4aa320461fc1d07b`
- Role: SVG rendering
- License: MIT

The upstream license remains in `library/lunasvg/LICENSE` and is copied to
`LICENSES/MIT-LunaSVG.txt`.

## Twire 2.12.3

- Upstream: `twireapp/Twire`, release `2.12.3`
- Role: reference behavior studied while implementing Twitch playback-token,
  embedded-player identity, Usher HLS, request-header and quality-resolution
  behavior
- License: GNU General Public License version 3

Twire is not bundled as an Android application or library. Its attribution is
preserved because it informed the independent twiNX resolver implementation.

## mpv

- Role: media playback engine
- Default upstream license: GPL version 2 or later; LGPL builds are possible
  only when configured without GPL-only files

The validated Nintendo Switch build uses the repository's patched
`switch-libmpv 0.36.0-7` recipe. The combined twiNX distribution is provided
under GPL-3.0-only.

## FFmpeg

- Exact twiNX 0.9.0 recipe: FFmpeg 7.1, `switch-ffmpeg` package release 12
- Role: demuxing, decoding, scaling and Nintendo Switch hardware acceleration
- Build configuration: `--enable-gpl`

The reproducible package recipe and patches are stored in
`scripts/switch/ffmpeg-libnx/`, including twiNX H.264-reference and NVTEGRA
hardware-frame transfer guards. GPL-2.0 and GPL-3.0 license texts are included
under `LICENSES/`.

## Other dependencies

twiNX also uses libcurl, libnx, devkitPro libraries, WebP and additional
components inherited from Switchfin and Borealis. Their source directories,
package metadata and upstream license notices remain applicable. Material icon
attribution is retained in `resources/material/LICENSE.txt`.

## Combined-project license

The root `LICENSE` contains GPL-3.0. This licensing choice covers the combined
twiNX distribution and original twiNX-specific code while preserving the more
permissive licenses of inherited Apache-2.0 and MIT components.
