<p align="center">
  <img src="images/twiNX_banner.png" alt="twiNX" width="100%">
</p>

<p align="center">
  <strong>A controller-first Twitch client for Nintendo Switch homebrew.</strong>
</p>

<p align="center">
  <a href="https://github.com/DomazinUS/twiNX/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/DomazinUS/twiNX?display_name=tag&sort=semver"></a>
  <img alt="Nintendo Switch" src="https://img.shields.io/badge/platform-Nintendo%20Switch-E60012">
  <img alt="License GPL-3.0" src="https://img.shields.io/badge/license-GPL--3.0--only-9147FF">
</p>

<p align="center">
  Created by <strong>HiroshiYamauchi</strong>
</p>

## About

**twiNX** is an independent Twitch client built specifically for Nintendo
Switch homebrew. It combines native playback, Twitch browsing and interactive
chat in an interface designed around Joy-Con, Pro Controller and touchscreen
use in handheld, tabletop and television play.

Version 0.9.0 introduces an automatic smartphone-style portrait experience and
embedded-player stream resolution that avoids Twitch's generic commercial-break
presentation in supported streams.

## Features

### Browse Twitch

- Built-in Twitch account sign-in
- Followed live channels and a separate offline-followed row
- Popular streams, top categories and live search by channel or game
- Current viewer counts in live search results
- Controller-friendly horizontal rows with pagination
- Channel artwork, stream thumbnails and account information

### Channel pages

- Live/offline status, banner, profile artwork and description
- Current stream and category information
- Recent broadcasts and clips
- Direct live, VOD and clip playback

### Playback

- Native Twitch live-stream, VOD and clip playback
- Source, transcoded and audio-only quality selection
- Embedded-player live resolution that avoids the generic commercial-break slate
- Software, hardware and Hybrid decoder modes
- Decoder changes apply immediately to the active live stream
- Hardened MPV/FFmpeg/NVTEGRA hardware-frame handling
- Experimental audio-reactive Joy-Con vibration with four response profiles
- Fullscreen, docked-chat and overlay-chat layouts
- Live streams do not create unnecessary MPV watch-later files

### Portrait mode

- Automatic landscape and portrait detection from either attached Joy-Con
- Clockwise and counter-clockwise portrait orientations
- Manual orientation choices when sensor control is unavailable
- Rotated 720 x 1280 touch surface with corrected touch coordinates
- Stream, live chat, persistent message draft and inline keyboard on one screen
- Touch keyboard/emote-sheet switching from the draft field
- Portrait emote sheet with inline emote previews in the message draft
- Adjustable touch-keyboard haptic feedback

### Live chat

- Read and send Twitch chat messages
- Twitch username colors and native badges
- Global, channel, subscriber and exclusive emotes
- Static and animated Twitch emotes
- Message composer with Recent, Channel and All emote tabs
- Touch-accessible Send button in the full composer
- Multi-line message wrapping
- Docked chat, full-height overlay and compact overlay
- Four-corner overlay placement and configurable width

### Controller-first interface

- D-pad and left-stick focus navigation
- Right-stick scrolling on long pages
- Handheld, portrait and television-friendly layouts
- Dedicated About page with features, credits and complete release history

## Installation

1. Download the latest release from the
   [twiNX Releases page](https://github.com/DomazinUS/twiNX/releases/latest).
2. Extract the release so the SD card contains:

   ```text
   /switch/twiNX/twiNX.nro
   ```

3. Launch twiNX from the Nintendo Switch Homebrew Menu.
4. Complete the built-in Twitch sign-in process.

No legacy `twinx.txt` configuration file is required.

## Updating

Replace the existing NRO in `/switch/twiNX/` with the newer release. OAuth,
chat, decoder, orientation and interface preferences remain under
`/config/TwiNX/` and are not stored inside the executable.

## Decoder modes

- **Software:** compatibility mode using CPU video decoding.
- **Hardware - Experimental:** NVTEGRA decoding with the validated twiNX frame,
  reference and transfer guards.
- **Hybrid - Experimental:** starts in hardware and can use software recovery
  during a disruptive playback transition.

The embedded-player resolver introduced in 0.9.0 prevents the generic Twitch
commercial presentation from entering the player in current testing, greatly
reducing the discontinuities that previously stressed hardware decoding.

## Known limitations

- Some standard Unicode emoji characters may render as missing-glyph boxes.
- Twitch may change its undocumented playback-token behavior without notice.
- Automatic portrait detection requires at least one compatible attached
  Joy-Con; manual orientation remains available from player settings.
- Hardware and Hybrid decoding remain experimental across untested stream formats.
- Audio-reactive Joy-Con vibration is experimental; its response may lag behind
  live audio depending on playback and output buffering.

## Building from source

See [BUILDING.md](BUILDING.md) for the supported MPV 0.36 package, guarded
FFmpeg 7.1/NVTEGRA recipe and Nintendo Switch build commands.

Quick build after dependencies are prepared:

```powershell
.\build-switch.ps1
```

The generated executable is:

```text
build_switch/TwiNX.nro
```

The historical build-target filename retains `TwiNX`; the visible application
name is **twiNX**.

## Project history

Major milestones:

- `0.1.0` - Initial native Twitch playback proof of concept
- `0.2.0` - Twitch sign-in and content browsing
- `0.3.0` - Channel details and quality selection
- `0.4.0` - Read-only live chat
- `0.4.2` - TV-style docked chat
- `0.4.8` - Decoder mode selector
- `0.5.0` - Expanded chat, badges and emotes
- `0.6.0` - Chat composer and emote picker
- `0.7.0` - Channel pages
- `0.7.1` - VOD and clip playback
- `0.7.9` - Compact four-corner overlay chat
- `0.8.0` - Offline followed channels
- `0.8.1` - Correct branding, About page and complete in-app history
- `0.9.0` - Portrait mode, embedded-player resolution and hardened hardware playback

See [CHANGELOG.md](CHANGELOG.md) for the full release history.

## Credits

- **HiroshiYamauchi** - twiNX creator and developer
- **Switchfin contributors** - original Nintendo Switch UI/player/platform base
- **Borealis contributors** - controller-first UI framework
- **Twire contributors** - reference behavior for embedded-player resolution
- **mpv, FFmpeg, libcurl, LunaSVG, libnx and devkitPro contributors**

Full attribution is available in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

The combined twiNX project and its original twiNX-specific code are distributed
under **GPL-3.0-only**. Inherited and third-party files retain their original
licenses and copyright notices. See [LICENSE](LICENSE),
[LICENSES](LICENSES/) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

twiNX is an independent, unofficial homebrew project. It is not affiliated
with, endorsed by or sponsored by Twitch, Amazon or Nintendo. Product names and
trademarks belong to their respective owners.
