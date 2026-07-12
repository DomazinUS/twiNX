# Changelog

All notable changes to **twiNX** are documented in this file.

twiNX is an independent, controller-first Twitch client for Nintendo Switch
homebrew, created by **HiroshiYamauchi**.

The format is inspired by [Keep a Changelog](https://keepachangelog.com/),
although early development releases are summarized from the available project
history.

---

## [0.9.0]

### Added

- Added a complete smartphone-style portrait player for handheld use.
- Added automatic orientation detection using either attached Joy-Con.
- Added clockwise and counter-clockwise portrait layouts plus manual orientation
  choices when automatic sensor control is unavailable.
- Added a rotated 720 x 1280 logical surface with inverse touchscreen-coordinate
  mapping.
- Added a portrait layout containing the live stream, live chat, persistent
  message draft and app-native touch keyboard on one screen.
- Added a portrait emote sheet that can replace the keyboard without closing the
  composer.
- Added two-way keyboard/emote-sheet switching by touching the message draft.
- Added inline emote images to the portrait message draft.
- Added adjustable touch-keyboard Joy-Con haptic feedback.
- Added an on-screen **Send** button to the full Twitch emote composer.

### Changed

- Live playback now requests Twitch's embedded-player identity. In validation,
  this bypasses the generic commercial-break presentation and continues directly
  with the broadcaster's stream.
- Promoted the tested portrait-mode work from an isolated experiment into the
  production twiNX application.
- Suppressed the left analog stick, stick click, `L`, `ZL`, `Minus` and the two
  inward-facing left Joy-Con buttons while in portrait mode to prevent accidental
  input.
- Restored all production metadata, OAuth data and preferences to the existing
  `twiNX` application and `/config/TwiNX/` paths.
- Updated Nintendo Switch application metadata to version `0.9.0`.
- Animated Twitch emotes are now treated as a supported chat option after
  successful console validation.

### Fixed

- Hardened the FFmpeg/NVTEGRA H.264 reference, decode-map and hardware-frame
  transfer paths against damaged or discontinuous Twitch media.
- Hardened MPV hardware-frame side-data handling.
- Prevented malformed decoder state from becoming a fatal null dereference or
  delayed heap-corruption crash.
- Preserved production playback and chat performance by keeping verbose playback
  diagnostics disabled in release builds.
- Prevented Twitch live streams from writing unnecessary MPV watch-later state.
- Retained the existing guarded hardware playback stack while removing the
  commercial presentation that most frequently triggered unsafe transitions.

---

## [0.8.1]

### Added

- Added a dedicated About page.
- Added a complete feature overview.
- Added the full available twiNX release history.
- Added project credits and third-party technology acknowledgements.
- Added an independent-project and non-affiliation notice.

### Changed

- Corrected visible application branding from `TwiNX` to `twiNX`.
- Updated Nintendo Switch application metadata:
  - Application name: `twiNX`
  - Author: `HiroshiYamauchi`
  - Version: `0.8.1`

### Fixed

- Enabled right-stick scrolling on the About page.
- Kept the About-page scrolling correction isolated from home-screen navigation.
- Preserved existing configuration paths to avoid breaking saved preferences
  and authentication data.

---

## [0.8.0]

### Added

- Added a dedicated **Followed channels — offline** row to the home screen.
- Added profile artwork, channel names and offline status to offline-channel cards.
- Added direct navigation from offline-channel cards to their channel pages.
- Added pagination support for followed channels.
- Added automatic exclusion of channels that are currently live.

### Changed

- Reorganized the home screen into:
  - Followed channels — live
  - Followed channels — offline
  - Popular live streams
  - Top categories
- Preserved Twitch’s followed-channel ordering.

---

## [0.7.9]

### Added

- Added a compact overlay-chat mode.
- Added configurable overlay-chat placement:
  - Top left
  - Top right
  - Bottom left
  - Bottom right
- Added independent overlay width and compact-height configuration.

### Fixed

- Fixed long chat messages being truncated after only a few lines.
- Added dynamic message-row height based on wrapped text, badges and emotes.
- Fixed chat-message overlap caused by mismatched measurement and drawing logic.
- Fixed the update package incorrectly rejecting Windows CRLF source files.

---

## [0.7.8]

### Added

- Added rendering support for incoming Twitch channel-exclusive and subscriber
  emotes, regardless of whether the signed-in viewer is entitled to send them.
- Added a **Chat emotes** preference:
  - Static
  - Animated
- Added animated-emote download and GIF frame decoding.
- Added static fallback while animated content loads.
- Added emote caching and resource limits intended to reduce memory pressure.

### Changed

- Kept the chat composer restricted to emotes the signed-in user is entitled to send.
- Marked animated-emote behavior as experimental.

### Fixed

- Added safe fallback behavior when an incoming emote image cannot be downloaded
  or decoded.

---

## [0.7.7]

### Changed

- Reworked playback ownership so only one active player controls the shared MPV
  instance.
- Live playback is now stopped and destroyed before opening a channel page.
- VOD and clip playback now starts with a clean fullscreen layout.
- Reset inherited live-chat video margins, pan, zoom and alignment before
  non-live playback.
- Moved channel-page default focus to permanent controls instead of recycled cards.

### Fixed

- Fixed live streams continuing to play and produce audio behind channel pages.
- Fixed VODs and clips inheriting an empty black live-chat area.
- Fixed stale player event handlers remaining active after navigation.
- Fixed channel-page focus becoming attached to detached or recycled media cards.
- Fixed cases where only the scrollbar responded after channel-page focus became stuck.
- Included the per-worker curl DNS-cache stability correction from 0.7.6.

---

## [0.7.6]

### Fixed

- Removed unsafe cross-thread sharing of libcurl’s DNS cache.
- Gave each HTTP worker its own private DNS cache.
- Fixed a race that could crash inside libcurl host-cache cleanup while badges,
  emotes or images were downloading.
- Improved background image-loading stability.

---

## [0.7.5]

### Changed

- Replaced automatic emote-grid focus routing with finite logical-index navigation.
- Made the selected emote index the authoritative navigation state.
- Up and Down now move by one complete emote row using index arithmetic.
- Up from the logical first row now exits to the active emote filter tab.
- Up from the filter tabs moves to the message draft.

### Fixed

- Fixed the chat composer becoming permanently trapped inside the emote grid.
- Fixed upward navigation failing after emote rows had been recycled.
- Fixed unloaded rows preventing navigation back toward the beginning of the list.
- Removed unreliable deferred focus behavior from emote navigation.

---

## [0.7.4]

### Changed

- Moved emote-boundary handling from individual recycled cells toward grid-level
  navigation.
- Added explicit routing from the first logical emote row to the active filter tab.

### Fixed

- Attempted to prevent recycled emote cells from retaining stale navigation state.

---

## [0.7.3]

### Changed

- Added explicit composer navigation routes between:
  - Message draft
  - Recent / Channel / All tabs
  - Emote grid
- Changed focus-border clipping to preserve horizontal card-border lines.

### Fixed

- Fixed cyan card borders losing their top and bottom lines.
- Improved upward materialization of emote rows.
- Improved transitions between the emote grid and composer controls.

---

## [0.7.2]

### Added

- Added the experimental **Hybrid** decoder mode.
- Hybrid mode starts with hardware decoding, temporarily falls back to software
  decoding during unstable transitions, and may restore hardware decoding after
  playback stabilizes.

### Changed

- Reworked upper home-screen rendering so scrolling cards remain behind the
  fixed header area.
- Reworked emote-grid navigation to materialize unloaded rows before moving focus.

### Fixed

- Reduced the risk of hardware-decoder crashes during Twitch commercial and
  presentation transitions.
- Fixed parts of the home-screen focus cursor drawing over the account and action header.
- Improved navigation through lazily loaded emote rows.

---

## [0.7.1]

### Added

- Added direct playback for Twitch VODs.
- Added direct playback for Twitch clips.
- Added Twitch media URL resolution before handing playback to MPV.
- Added playback-error fallback to the media metadata dialog.

### Changed

- Reused the existing twiNX player for live streams, VODs and clips.
- Preserved existing player controls, quality handling and decoder preferences.

### Fixed

- Improved emote-grid upward navigation.
- Reduced home-screen scrolling that caused cards to move into the upper header area.

---

## [0.7.0]

### Added

- Added detailed Twitch channel pages.
- Added channel banner and profile artwork.
- Added live/offline channel status.
- Added channel description and stream metadata.
- Added channel schedule section.
- Added recent-broadcast cards.
- Added clip cards.
- Added channel-category cards.
- Added a channel shortcut from live playback.
- Added direct **Watch live** behavior for channels that are currently online.

### Changed

- Opening an offline channel now goes directly to its channel page.
- Opening a live channel continues to start live playback.
- Channel media cards initially opened detailed metadata while direct playback
  was being developed.

---

## [0.6.0]

### Added

- Added the Twitch chat message composer.
- Added an on-screen emote picker.
- Added Recent, Channel and All emote filters.
- Added controller-first message entry and emote insertion.
- Added sending messages directly from the player.
- Added the ability to browse large emote inventories.

### Changed

- Expanded live chat from read-only viewing into interactive chat participation.

---

## [0.5.4]

### Fixed

- Added image content-type validation before attempting image decoding.
- Prevented invalid server responses from being treated as badge or emote images.
- Reduced crashes caused by malformed, redirected or non-image HTTP responses.

---

## [0.5.3]

### Added

- Added native Twitch chat badges.
- Added broadcaster, moderator, subscriber and other supported badge rendering.
- Added the Plus-button shortcut for switching chat layouts.

### Changed

- Improved chat identity presentation with usernames, colors, badges and emotes.

---

## [0.5.2]

### Fixed

- Corrected live-chat text measurement and wrapping.
- Fixed messages overlapping adjacent rows.
- Improved alignment between badges, usernames, text and emotes.
- Improved multi-line chat readability.

---

## [0.5.1]

### Fixed

- Improved chat and emote rendering stability.
- Added safer handling for missing or unavailable emote images.
- Reduced crashes during asynchronous chat image loading.
- Improved cleanup of chat-related resources.

---

## [0.5.0]

### Added

- First substantial Twitch chat feature release.
- Added richer chat-message rendering.
- Added Twitch emote support in received messages.
- Added username colors and message formatting.
- Added asynchronous badge and emote image loading.
- Added chat scrolling and message-history handling.

### Changed

- Expanded the original read-only chat prototype into a more complete live-chat
  implementation.

---

## [0.4.8]

### Added

- Added a decoder-mode selector.
- Added selectable software and hardware decoding modes.
- Added an experimental label and warning for hardware decoding.

### Changed

- Made decoder behavior configurable from the player settings instead of being
  fixed at build time.

---

## [0.4.7]

### Changed

- Improved transitions into and out of hardware decoding.
- Improved decoder reinitialization when Twitch changed stream presentation.
- Reduced visual disruption during decoder recovery.

### Fixed

- Fixed additional hardware-decoder transition failures.

---

## [0.4.6]

### Fixed

- Improved Twitch playback stability when using software decoding.
- Added recovery behavior for hardware-decoder failures.
- Improved handling of decoder resets and stream reconfiguration.
- Reduced crashes and lockups following commercial breaks or playlist changes.

---

## [0.4.5]

### Fixed

- Corrected video sizing in TV-style docked-chat mode.
- Preserved the stream aspect ratio when the player area was reduced.
- Fixed stretching and cropping caused by the docked chat panel.

---

## [0.4.4]

### Added

- Added MPVCore-level Twitch advertisement and stream-transition recovery.
- Added lower-level playback recovery independent of the visible player controls.

### Fixed

- Improved recovery after Twitch replaced or interrupted stream segments.
- Reduced cases where audio or video failed to resume after a transition.

---

## [0.4.3]

### Added

- Added explicit commercial-break recovery.
- Added detection and recovery for playback interruptions around Twitch ads.

### Fixed

- Improved automatic return to the live stream after commercial breaks.
- Reduced black-screen and stalled-playback conditions after advertisements.

---

## [0.4.2]

### Added

- Added TV-style docked chat.
- Added a dedicated right-side live-chat panel.
- Added reduced-width video layout while docked chat is visible.
- Added a persistent Send message control area.

### Changed

- Adapted the player layout for simultaneous video and chat viewing on television
  and handheld displays.

---

## [0.4.1]

### Changed

- Moved chat rendering behind player controls.

### Fixed

- Fixed chat covering playback controls and other player UI elements.
- Improved layering between video, chat and player overlays.

---

## [0.4.0]

### Added

- Added read-only Twitch live chat.
- Added real-time Twitch chat connection.
- Added incoming chat-message display beside live playback.
- Added basic usernames and message text.
- Added chat visibility controls.

---

## [0.3.2]

### Fixed

- Reset the home-screen scroll position to the absolute top.
- Fixed the home screen reopening at an unexpected vertical position.
- Improved initial focus placement when returning home.

---

## [0.3.1]

### Added

- Added a television-oriented home-screen layout.
- Added player quality information.
- Added improved live-stream cards and category presentation.
- Added source-quality identification in the player.

### Changed

- Improved the home-screen structure for controller and television use.

---

## [0.3.0]

### Added

- Added detailed live-channel information.
- Added stream title, game/category and viewer information.
- Added playback-quality selection.
- Added Twitch stream-variant discovery.
- Added source and transcoded-quality choices.

### Changed

- Expanded live playback beyond a single automatically selected stream URL.

---

## [0.2.5]

### Fixed

- Added deterministic controller-focus routing.
- Fixed unpredictable cursor jumps between home-screen elements.
- Improved navigation consistency across horizontal rows.
- Reduced focus changes when no directional input was being made.

---

## [0.2.4]

### Fixed

- Improved home-screen focus behavior.
- Corrected application and interface icons.
- Fixed incorrect or inconsistent controller-navigation routes.
- Improved selected-card highlighting.

---

## [0.2.3]

### Added

- Added early twiNX branding.
- Added improved controller-navigation structure.
- Added clearer home-screen sections and action controls.

### Changed

- Began transitioning the original proof of concept into a dedicated Twitch client.

---

## [0.2.2]

### Fixed

- Improved browsing stability.
- Reduced crashes while loading Twitch rows and images.
- Added safer handling for incomplete Twitch responses.
- Improved navigation while content was still loading.

---

## [0.2.1]

### Fixed

- Added early user-experience improvements.
- Improved initial focus and controller behavior.
- Improved home-screen spacing and status presentation.
- Corrected minor login and browsing workflow issues.

---

## [0.2.0]

### Added

- Added Twitch sign-in.
- Added authenticated Twitch API access.
- Added followed-channel browsing.
- Added popular live-stream browsing.
- Added top-category browsing.
- Added search.
- Added signed-in account information.
- Added sign-out and refresh controls.

### Changed

- Replaced the original fixed-channel configuration workflow with built-in
  authentication and interactive browsing.

---

## [0.1.0]

### Added

- Initial proof of concept.
- Added native Twitch live-stream playback on Nintendo Switch.
- Added basic MPV integration.
- Added an initial fixed-channel playback workflow.
- Added early controller support.
- Established the Switchfin/Borealis-based application foundation.

---

## Project credits

- **twiNX** was created and developed by **HiroshiYamauchi**.
- twiNX is built using open-source technologies including Borealis, MPV, FFmpeg,
  libcurl and related libraries.
- Portions of the original application foundation derive from upstream open-source
  projects whose licenses and notices are preserved separately.

twiNX is an independent project and is not affiliated with, endorsed by or
sponsored by Twitch Interactive, Nintendo, Amazon or their subsidiaries.
