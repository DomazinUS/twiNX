# twiNX Portrait Lab

This branch is an isolated experiment for a portrait-oriented handheld player.
It is not the production twiNX release line.

## Isolation rules

- Production checkpoint: `stable/0.8.1-current`
- Experiment branch: `experiment/portrait-mode`
- Experiment worktree: `C:\Repository\twiNX\twiNX-portrait-experiment`
- Output: `build_switch/TwiNXPortraitExperimental.nro`
- Display name: `twiNX Portrait Lab`
- Title ID: `010ff000ffff0006`
- Runtime data: `/switch/TwiNXPortraitExperimental`
- Twitch credentials/settings: `/config/TwiNXPortraitExperimental`

Do not merge the experiment into the stable branch until portrait rendering,
touch-coordinate transforms, inline keyboard behavior, and playback stability
have all passed console testing. Stable fixes may be cherry-picked into this
branch deliberately.

## Milestones

1. Joy-Con accelerometer detection with filtering, hysteresis, and manual modes.
2. Rotated 720 x 1280 logical surface with inverse touch-coordinate mapping. (prototype implemented)
3. Portrait player layout: stream, chat, persistent draft, and app-native keyboard. (prototype implemented)
4. Controller-detached manual rotate control and persistence.
5. Performance and long-session playback validation.

The first two milestones now have an experimental player-only prototype.
Landscape remains unchanged; portrait currently shows video above a full-width
chat panel, persistent draft area, and touch/controller keyboard.
