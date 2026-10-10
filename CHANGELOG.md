# Changelog

All notable changes to Nitshot. Versions before 0.8.0.0's rename were released as
*BetterSnippingTool*; settings carry over automatically (see the README).

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed
- **Memory no longer piles up while the program waits.** After a snip, the
  screen-readout buffer (66 MB at 4K HDR) stayed allocated, and the graphics
  driver kept freed copies of it cached, so every recording, HDR switch or
  display change added another 64 MB - up to about 180 MB for a program sitting
  in the tray. The buffer is now released and the driver's cache handed back a
  few seconds after each snip, and capture rebuilds trim before they release.
  Idle memory after a snip: 115 MB → 53 MB, with snips exactly as fast as
  before.

### Added
- After 5 minutes without a snip, the capture machinery is released entirely,
  bringing idle memory down to about 17 MB; the next snip takes about 0.1 s
  longer, once. `IdleReleaseMinutes` in the INI changes the delay (0 = never).

## [0.8.2.0] - 2026-10-10

### Changed
- **Recordings keep their frames on the GPU.** The capture is handed to the
  hardware encoder as GPU textures instead of making a round trip through system
  memory. On the 8-bit paths the RGB-to-NV12 conversion now runs on the GPU too;
  on the HDR path the PQ conversion renders straight into the encoder's P010
  texture. At 4K and 30 fps, the recorder's own work fell from 9.2 to 0.08 ms per
  frame for HDR and from 5.8 to 0.04 ms for 8-bit; CPU use from 35 % to 1 % and
  from 205 % to 3 % of a core. Falls back to the previous path by itself where
  the hardware does not support this; `RecordZeroCopy=0` forces the fallback.
- HDR light-level metadata (MaxCLL/MaxFALL) is now measured on the GPU, with
  results identical to the previous CPU measurement.
- The keyboard hook runs on a thread of its own, so nothing else the program does
  can delay it long enough for Windows to hand Win+Shift+S to the Snipping Tool.
- The overlay comes up about 200 ms faster on every snip: it reuses the capture's
  graphics device and compiles its shaders once, at startup, instead of building
  both each time - at no extra memory cost while idle.
- Freeform selections redraw three to five times faster while being drawn.
- Saving copies the encoded image less often, and the plain-DIB clipboard format
  is only built when the snip has transparency.
- The diagnostic log is moved aside at 4 MB instead of growing without limit.

### Fixed
- Two snips taken within the same second no longer overwrite each other, and an
  HDR snip's PNG copy can no longer overwrite another snip's file.
- If the overlay's graphics setup failed, the whole program quit. It now just
  cancels that snip.
- After a display change or a lost screen capture, snips no longer stay on the
  slow fallback capture until the next restart.
- Switching the language while a recording or a save was in progress could read
  freed memory.
- Stopping audio capture could release objects its thread was still using.
- Changing the save folder in the settings while a save or recording was running
  could hand it a half-written path; both now work from a copy of the settings
  taken when they start.
- Long log lines in non-Latin scripts were dropped.

### Security
- The shader compiler and other system DLLs are loaded from System32 only, and
  the executable resolves its own imports from System32: a DLL placed next to
  `Nitshot.exe` (for example in a Downloads folder) is no longer picked up.
- A language setting that looks like a path is refused instead of being used to
  load a file from outside the `langs` folder.
- Stricter bounds checks when patching MP4 metadata, and a cap on retries when
  the display configuration keeps changing.

## [0.8.1.0] - 2026-10-10

### Fixed
- Recording kept running on a frozen frame after a display-mode change, a UAC
  prompt or a game switching to exclusive fullscreen, and leaked graphics memory
  each time. The capture is now rebuilt on the same device; a recording whose
  graphics device is lost altogether stops and keeps what it captured.
- Stopping a recording whose file took longer than 15 seconds to finish could
  crash.
- Pressing the hotkey while a recording was starting or finishing could corrupt
  the recorder.
- When rebuilding a recording's sound from its WAV failed part-way, the truncated
  result replaced the original and the WAV - by then the only full copy of the
  sound - was deleted.
- Quitting during a slow HDR save left a truncated `.jxr`; exiting now waits for
  pending saves.
- A key held across the hook's periodic refresh could reach the foreground
  window unpaired.

## [0.8.0.0] - 2026-09-21

### Added
- English and German interface, following the Windows display language.
  Further languages can be added by copying one text file; untranslated lines
  fall back to English.
- HDR recordings carry the light-level metadata (MaxCLL/MaxFALL, mastering
  display) players need to show them at the right brightness.
- Tabbed settings window with an Apply button, and a configurable folder for
  recordings.
- User guide in English and German.

### Changed
- Renamed from BetterSnippingTool to **Nitshot**. Settings, autostart and
  translations move over automatically on first start.
- 8-bit recordings of an HDR display are tone-mapped from the floating-point
  capture, so they no longer come out about three times too bright and washed
  out.
- With the WAV sidecar used as the recording's sound source, the `.wav` is
  removed once its sound is in the MP4, so players do not offer the sound twice.

### Fixed
- The overlay could vanish a fraction of a second after appearing, as if Escape
  had been pressed, when Windows took focus back on releasing the Windows key.
- Under remote desktop the overlay could show a black screen with selection
  trails, because a fresh screen capture handed over an empty first frame.
- An unchanged custom snip folder was reset to the default every time the
  settings were confirmed.
- The diagnostic log setting only took effect after a restart.

## [0.7.0.0] - 2026-09-20

### Added
- **10-bit HDR recording**: on an HDR display, HEVC Main10 in BT.2020 with the
  PQ curve, so recordings keep the colours and highlights seen on screen.
- The available video encoders are found by writing a test file with each one,
  not just by asking the driver, and the best working one is chosen
  automatically; it can also be picked in the settings.
- Optional lossless WAV copy of a recording's sound, and the option to build the
  MP4's sound from it.
- The settings window opens centred and remembers where it was moved to.

### Removed
- The shutter sound after each snip.

## [0.6.0.0] - 2026-09-19

### Added
- **Screen recording** of a selected area to MP4 (H.264 + AAC), with system
  sound or microphone, a recording frame and a stop bar. The hotkey stops a
  running recording.

### Fixed
- Recordings of a quiet desktop had no sound track, and sound jumped after
  pauses: silence is now filled in so audio and video stay aligned.
- Freeform mode drew a selection from the corner of the screen before the first
  click.

## [0.5.0.0] - 2026-09-18

### Added
- Settings window, following the Windows light or dark setting.
- Start with Windows, and a notification naming the saved file.

## [0.4.0.0] - 2026-09-18

### Added
- **HDR screenshots**: on an HDR display the capture is floating point and is
  saved as JPEG XR, with a tone-mapped PNG beside it; the clipboard gets an
  accurate 8-bit copy.

## [0.3.0.0] - 2026-09-18

### Added
- The selection overlay: the frozen, dimmed desktop with a toolbar for
  rectangle, freeform, window and full-screen snips, across all monitors.

## [0.2.0.0] - 2026-09-18

### Added
- Screen capture via Desktop Duplication with a GDI fallback; snips go to the
  clipboard and to a PNG file at the same time.

## [0.1.0.0] - 2026-09-18

### Added
- Tray program that takes over Win+Shift+S and PrtScn while it runs, leaving
  the Windows Snipping Tool installed and untouched.
