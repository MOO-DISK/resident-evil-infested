# Match bumpers

Optional files live beside the executable, independent of the configured game
asset directory. Windows builds and the Linux packager copy these files from
this repository's `infested` folder when present.

```
infested/
  audio/intro.wav
  video/
    intro/chris.mp4
    intro/jill.mp4
    intro/barry.mp4
    intro/rebecca.mp4
    intro/richard.mp4
    intro/enrico.mp4
    victory/chris.mp4
    victory/jill.mp4
    victory/barry.mp4
    victory/rebecca.mp4
    victory/richard.mp4
    victory/enrico.mp4
    wesker_victory.mp4
```

Export silent MP4 files with
H.264, 8-bit `yuv420p`, constant 24 fps (30 fps also works). Recommended size:
1280 x 720. Intro files should be exactly three seconds (72 frames at 24 fps).
Export the shared intro sound as PCM WAV, ideally 16-bit stereo.

The match-start barrier waits for the frozen survivor roster and each copy's
preloaded videos, plays the intro for three seconds, and releases gameplay only
after every connected copy finishes. Networking continues throughout. Rejoining
an existing game does not replay its intro. The match clock starts afterward.

Panels appear in seat order: one survivor fills the screen, two split it in
half, three split it into thirds. The AI-director mode can include the host as
a fourth survivor. Dividers slope 15 degrees from vertical (clamped on unusually
narrow windows to keep panels visible). Videos are cropped centrally to fill
upright panels, never stretched. Keep faces/actions near the centre and leave
background around them for narrow panels.

On a confirmed survivor escape, victory panels use the starting survivor roster,
including teammates who died. On all-survivors-dead, Wesker plays full screen.
Time expiry, connection failure and impossible-escape outcomes retain their
existing results screen. End clips play until the longest finishes (maximum
30 seconds); shorter clips hold their last frame. Results-screen timers begin
when the ending clip finishes. Only the intro plays `intro.wav`.

Missing or unsupported videos leave their assigned panel black; if all videos
are absent, the visual is skipped. The intro still calls its sound and observes
the start barrier. Missing audio is silent. `[bumper]` debug log lines report
assets loaded/skipped and playback completion. All multiplayer copies must use
network protocol version 71 or newer compatible builds.

Manual verification: test one, two and three survivors; confirm character clips,
angles, centre cropping, shared sound and input lockout; then test a survivor
escape, all-survivors-dead, missing files and reconnecting mid-match. Run with
`RE1_DEBUGLOG=1` and retain `re1_debug.log` if playback or the barrier fails.

Decoder integration check: `tests/test_bumper_video.cpp` is a standalone console
test. Supply a short silent 24 fps H.264/yuv420p fixture; it checks three
simultaneous readers, shared-clock frames, EOF retention, cleanup, missing
assets, isolation from the existing frame reader, and start-barrier phases.
It does not initialize or launch the game.
