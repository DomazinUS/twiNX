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
2. Rotated 720 x 1280 logical surface with inverse touch-coordinate mapping.
3. Portrait player layout: stream, chat, message composer, inline keyboard.
4. Controller-detached manual rotate control and persistence.
5. Performance and long-session playback validation.

The first milestone is implemented. Until milestone 2, orientation changes are
reported and persisted but do not rotate the framebuffer.
