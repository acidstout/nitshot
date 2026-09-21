# Nitshot

**Version 0.8.0.0 — complete**
Copyright © 2026 Rekow IT

A small native Win32 replacement for the Windows 10 Snipping Tool: a tray-resident
C17 program with no runtime dependencies that takes over `Win+Shift+S` and `PrtScn`,
freezes and dims the desktop, and puts the result on the clipboard *and* in
`Pictures\Nitshot`. On an HDR display it captures in floating point and
saves real HDR files.

On first start, Nitshot moves `%APPDATA%\BetterSnippingTool` to `%APPDATA%\Nitshot`, carries the
settings over from the old INI section and re-registers autostart under the new name.
Snips already saved in `Pictures\BetterSnippingTool` stay where they are; new ones go
to `Pictures\Nitshot`.

**User guide:** [English](docs/UserGuide.en.md) · [Deutsch](docs/Benutzerhandbuch.de.md). This README is the technical reference.

# Usage

Press the hotkey, the desktop freezes and dims, and the toolbar offers
**rectangle · freeform · window · fullscreen · record · close**. On a display with
advanced colour on, stills are floating point end to end and are written as JPEG XR.
Everything is reachable from the tray icon, including a settings window.

Measured on a 3840×2160 desktop: **grab 7–16 ms**, PNG encode ~200 ms for the full
screen and under 1 ms for a typical snip. The grab is what gates the overlay, so
the encode being the slow half does not matter — it happens after the user already
has their selection.

## The overlay

One borderless window across the whole virtual desktop, drawn with D3D11 so that
phase 4 can switch the swap chain to a floating-point format and show HDR content
as it really looks. There is no vertex buffer and no input layout anywhere: every
draw is a single quad built from `SV_VertexID`, with the destination and source
rectangles coming from a constant buffer. Four small shaders, compiled at startup
through `d3dcompiler_47.dll`, cover the entire UI.

| Mode | Behaviour |
|---|---|
| Rectangle | drag; captures on release |
| Freeform | draw a closed shape; the area outside the outline is transparent |
| Window | highlights the window under the cursor, click to take it |
| Fullscreen | takes everything immediately |
| Record | drag an area to record instead of capture; see below |

`Esc`, right-click or the toolbar's ✕ cancels; `1`–`4` pick a capture mode directly. The
mode is remembered for next time, the way the Snipping Tool does it — except
fullscreen, which is a one-off action rather than a mode worth returning to.

**Freeform** shares one rasteriser between the live preview and the finished cut
(`poly.c`): the preview uploads the coverage mask to the GPU as the outline grows,
and the saved image multiplies its alpha by the very same mask. A second
implementation would eventually disagree with the first about an edge pixel. The
alpha is premultiplied, so pasting a cut-out onto a light background shows no dark
fringe.

**Window mode** walks the top-level windows in z-order rather than calling
`WindowFromPoint`, which would only ever return our own overlay, and highlights
`DWMWA_EXTENDED_FRAME_BOUNDS` so the selection matches what the user sees instead
of including the invisible resize border. Cloaked (background UWP) windows are
skipped.

**The toolbar** is rendered on the CPU into a premultiplied BGRA texture. GDI has
no anti-aliasing, so each layer is drawn white-on-black at four times the final
size and box-filtered down. That is what gives the icons clean edges without
dragging in Direct2D — which the Windows SDK does not expose to C at all, its
headers declaring the interfaces as opaque types with no vtables — or a font.

**The overlay defends its activation for its first 800 ms.** It used to close
the instant it lost activation, and occasionally did so 140–250 ms after
appearing — with no key and no second hotkey in between, per the log. That is
the moment the Windows key is released, and the shell reacting to the release
took activation away. To the user it looked like a phantom Escape. Losing
activation that early is never a deliberate switch, so the overlay now takes it
back (a bounded four times, posted rather than done inside `WM_ACTIVATE`, where
the two windows would trade activation). Reproduced on demand with a window that
steals activation at a chosen delay: at 125 ms the overlay stays; at 1250 ms it
closes as before. The log now names the window that took over, by class and
process — `WM_ACTIVATE` does not, when that window belongs to another process.

Two things guard against the worst way this program could fail, an overlay stuck
on screen over everything:

- a 250 ms watchdog closes it if it is ever not the foreground window, because
  another process can take the foreground without our window seeing `WM_ACTIVATE`;
- the hotkeys stay hooked while the overlay is up but do nothing, so a second
  Win+Shift+S cannot fall through to the shell and launch Snip & Sketch on top of
  our own overlay.

## Capture

`Capture_VirtualDesktop()` composites every attached output into one
virtual-desktop image, then falls back wholesale if any output fails — a
half-black desktop would be worse than a slower correct one.

- **DXGI Desktop Duplication** is the fast path. The D3D11 device and the
  duplication objects are built at startup and kept warm, so a hotkey press pays
  for none of that. A static desktop produces no new frames, so the first
  `AcquireNextFrame` usually times out; the duplication object is then rebuilt,
  because a fresh one always delivers the current desktop as its first frame.
- **GDI `BitBlt`** is the fallback, for indirect and virtual displays where
  duplication is unavailable, and for **rotated** outputs — duplication hands
  those out in the panel's native orientation, and GDI simply reads them the
  right way round. `ForceGdi=1` in the INI pins this path for testing.

## HDR

When any display has advanced colour on, the whole pipeline switches to
floating point: Desktop Duplication hands over `R16G16B16A16_FLOAT` via
`DuplicateOutput1`, the overlay's swap chain is float with
`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`, and the snip is written as **JPEG XR**
(`64bppRGBAHalf`) — the only floating-point format the in-box WIC codecs can write
at all. Toggling HDR on or off rebuilds the capture machinery on the next snip.

Windows composes such a display in **scRGB**: linear, Rec.709 primaries, 1.0 = 80
nits by definition. SDR white does not sit at 1.0 but wherever the *SDR content
brightness* slider put it, so that level is read from `DISPLAYCONFIG_SDR_WHITE_LEVEL`
rather than assumed. Verified against a known test pattern on a 240-nit display:

| authored | captured | tone-mapped back |
|---|---|---|
| white 255 | scRGB 3.0000 | 255 |
| grey 128 | scRGB 0.6470 | 128 |
| black 0 | scRGB 0.0000 | 0 |

exactly `SrgbToLinear(128/255) × 3`, and 240 nits = scRGB 3.0 as the API said.

**The 8-bit copy forces a choice**, because keeping SDR white at 255 and
representing anything brighter are contradictory demands — once white is 255 there
are no code values left above it. The default **clips**: SDR content is bit-exact
and highlights become white, which is what someone pasting a screenshot expects,
and the `.jxr` beside it still holds the real values. `HdrSdrRollOff=1` instead
trades the top of the range for highlight detail (white lands near 231, and 1×, 2×
and 4× white become 231, 246 and 254).

**JPEG XR quality is not lossless by default.** Measured on a busy 3840×2160 frame,
lossless takes ~19 s and produces 24 MB; quality 90 takes ~2.5 s and 8 MB, with no
visible difference on screen content. `HdrJxrQuality=100` selects true lossless.

## Output

The PNG is encoded **once** and feeds both destinations; encoding a 4K screen
twice cost more than the capture itself. The clipboard gets the registered `PNG`
format plus `CF_DIBV5` and `CF_DIB`, so both modern and old Win32 apps find
something they understand. The PNG filter is pinned to `Sub` rather than WIC's
adaptive default, which on 8 megapixels dominated the whole snip for a few per
cent of file size.

`CF_DIB` has no alpha, so a freeform cut-out is composited **over white** for that
one format — otherwise anything that does not understand `CF_DIBV5` or `PNG` pastes
the shape on a black rectangle. The pixels are premultiplied, so that composite is
just adding back the part the shape does not cover.

**All of it runs on a worker thread.** That is not an optimisation; see the hotkey
note below.

## Recording

The toolbar's record button switches the overlay to picking an area to record
rather than to capture. Drag one out and recording starts: a red frame marks the
area and a small bar shows the elapsed time with a stop button. **Win+Shift+S or
PrtScn also stops it** - while a recording runs, that is what the hotkey means.
The result is an H.264 + AAC **MP4 in `Videos\Nitshot`**, or in the folder set under Settings → General → Videos.

Verified by decoding a recording back: 960×540 at exactly 30.0 fps, 168 frames over
5.567 s, with the audio timeline matching the video to within a millisecond, and a
captured test tone coming back at 5838 against the 6000 it was played at.

- **Sound** is one source at a time: the speakers (a WASAPI loopback of the default
  playback device) or the microphone. Mixing both would mean resampling and
  drift-correcting between two independent clocks, which is a bigger job than it
  looks and is left out.
- A loopback capture delivers *nothing at all* while the machine is silent - not
  zeroed packets, no packets. The video loop therefore tops the audio timeline up
  with real silence whenever it falls behind, or a recording of a quiet desktop
  would have no audio track and a recording whose sound paused would jump.
- **A loopback capture is a tap on the *processed* signal**, and on a device with
  enhancements switched on that is audible. Measured here against a flat
  ten-tone reference played through the default endpoint (an EPOS GSX 300),
  what came back was bent into a smile - `+17.9 dB` at 94 Hz, `+8.3` at 246,
  `+4.2` at 504, `0` at 996, `−1.9` at 2004, `+4.7` at 3996, `+9.8` at 8004,
  `+8.9` at 12000, `+10.4` at 15996, `+7.6` at 19922 - before the recorder ever
  saw it. Playing such a recording back through the same endpoint applies the
  curve a second time, which is what makes it sound damp. The codec is not at
  fault: content still reaches 19.9 kHz and nothing clips (peak `−2.3 dBFS`,
  zero samples at full scale).

  **`RecordWavSidecar`** writes the same PCM untouched to a `.wav` beside the
  `.mp4`. Media Foundation's AAC encoder stops at 192 kbps — measured, 96 / 128 /
  160 / 192 are the only rates it offers for 48 kHz stereo — and stream copy is
  not possible in principle here, because a loopback capture hands over decoded
  PCM and there is no source bitstream to remux. The sidecar is fed from the same
  place as the encoder, silence padding included, so it stays sample-aligned with
  the video. Measured on the same recording: the sidecar's tone levels match the
  raw loopback capture to the second decimal, while the AAC track drifts by up to
  0.05 dB and picks up a 9 ms gap from encoder priming that the WAV does not have.

  **`RecordMuxFromWav`** goes one step further: nothing is encoded to AAC while
  recording at all. The capture writes PCM to the WAV, the MP4 gets video only,
  and afterwards the WAV is encoded once and muxed in (`remux.c`, video copied
  through compressed — never re-encoded). This is not a quality trick and does not
  pretend to be: measured against its own sidecar the result matches within
  0.05 dB and still carries AAC's 9 ms priming gap, because it is the same encoder
  over the same samples. What it buys is that no part of the audio path has to keep
  up with anything while recording — no encoder running against the capture clock,
  no timeline to pad — so that whole class of live-timing fault cannot arise, and
  the WAV is the authoritative copy. It cost 15–47 ms on the recordings tested.

  With this on the `.wav` is an **intermediate and is deleted** once its sound is
  in the MP4. Keeping it looked tidy and was not: players that auto-load a
  same-named sidecar — MPC does — then offer the same audio twice, as two
  selectable tracks. If the mux fails the `.wav` is kept, because at that point
  it is the only place the sound still exists.

  Two Media Foundation defects had to be worked around to make it produce a valid
  file, both found by reading the bytes that came out:

  - A source reader's video media type reports the **coded** frame size, which for
    HEVC is padded up to whole coding units — a 720-line recording reads back as
    736. Handing that type to the sink writes 736 into `tkhd`, and players show
    sixteen rows of padding. The real size is in `MF_MT_MINIMUM_DISPLAY_APERTURE`.
  - An MP4 source offers its codec private data as
    `MF_MT_MPEG4_SAMPLE_DESCRIPTION` containing the sample *entry* (the `hvc1`
    box), while the MP4 sink writes that blob into the place the **`stsd` box**
    belongs. Pass it back unaltered and the file ends up with `hvc1` sitting
    directly inside `stbl` — a video track Media Foundation itself can no longer
    find. Wrapping the entry in an `stsd` box before handing it over is the fix.

  `RecordRawAudio` asks for `AUDCLNT_STREAMOPTIONS_RAW`, which bypasses the
  endpoint's effects on drivers that support it. This one does not - it answers
  `AUDCLNT_E_RAW_MODE_UNSUPPORTED` (`0x88890027`) and the two captures come back
  identical to the second decimal. On a device like that the only fix is outside
  this tool: turn the endpoint's enhancements off, or flatten its EQ preset. The
  log says which of the two happened on every recording.
- **Only the front pair is kept** when the endpoint's mix is multi-channel. That
  endpoint presents 7.1 (`8 ch`, mask `0x63F`), and a stereo source measured
  `−11.4 dBFS` in channels 0 and 1 with the other six at digital zero, so nothing
  is lost in practice; a genuine 5.1 source would lose its centre and surrounds.
  A correct fold-down needs the channel mask and matrix coefficients and is left
  out deliberately.
- **Plain `DuplicateOutput` is not a usable SDR capture on an HDR display.** It
  hands the scRGB desktop back as 8-bit *without* dividing by the SDR white
  level, so on a 240-nit white everything arrives three times too bright and
  everything above a third of white clips. Measured against a known ramp played
  back full screen: authored 32 came out at 10.5 nits instead of 3.5, and every
  bar from 160 upwards sat at 239.7 - flat white. That is why an 8-bit
  recording of an HDR desktop looked washed out.

  So the capture is **always floating point when the display is in HDR**,
  whatever gets written. An 8-bit recording is then tone-mapped on the GPU with
  the same curve the still-image path uses - divide by SDR white, optional
  highlight roll-off, sRGB encode. Re-measured the same way: every bar within
  one code value of where it should be, 0 to 255, and the primaries come back
  at exactly scRGB 3.0 in their own channel with no crosstalk.

- **Recording follows the display into HDR.** On a display in advanced colour the
  recorder asks `DuplicateOutput1` for scRGB half-float, converts it on the GPU to
  10-bit BT.2020 PQ (`hdrvideo.c`: one full-resolution pass for luma, one
  half-resolution pass that averages 2×2 chroma after the transfer function, as
  4:2:0 is defined to) and encodes P010 through HEVC Main10. Verified against PQ
  computed independently on the CPU — SDR white (80 nits) lands on luma code 489,
  400 nits on 635, 1000 nits on 722, 10000 nits on 940, every one within a code of
  the curve, with neutral chroma sitting exactly on 512.

  Before this, recording used plain `DuplicateOutput`, which on an HDR display
  hands over the desktop the compositor has *already* flattened to 8-bit. That is
  where the washed-out look came from; it was never the codec.
- **The encoder list is probed, not enumerated.** `MFTEnumEx` says what the driver
  advertises, which is not the same as what the MP4 sink will take: the
  `NVIDIA AV1 Encoder MFT` here enumerates, configures and starts, then fails at
  `Finalize` with `MF_E_SINK_HEADERS_NOT_FOUND`, because the in-box MP4 sink
  cannot mux AV1. So `encoder.c` builds the list by writing and finalising a
  one-frame file per candidate through the very same configuration code
  recording uses. On this machine that yields H.264 8-bit, HEVC 8-bit and
  HEVC Main10, all on hardware, in 454 ms — cached in the INI against the
  adapter and its driver version, and re-probed only when either changes.

  Hardware encoding was already in use before any of this:
  `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` makes Media Foundation route to
  whichever vendor MFT is installed — NVIDIA, Intel or AMD.
  Direct coding against the NVENC, AMF and Quick Sync SDKs directly would mean three
  vendor SDKs to land exactly where `MFTEnumEx` already lands.

- **"Best available" is not "newest".** On an HDR display HEVC wins outright,
  being the only way to carry HDR here at all. On an SDR one H.264 is preferred
  even though it is older and larger. Windows 10 needs the HEVC Video Extensions
  package to *play* HEVC back, and plenty of upload targets reject it.
  `RecordEncoder` overrides the ranking; a choice that stops probing clean falls
  back down the list and says so in the log.

- **The container is stamped afterwards.** Media Foundation's MP4 sink writes no
  `colr` box, so nothing outside the HEVC bitstream said what the picture was, and
  a player trusting the container reads BT.2020 PQ as Rec.709 — the washed-out
  look again, by another route. `mp4tag.c` inserts the box after `Finalize`. That
  is only safe because the sink puts `moov` last, so growing it leaves every
  `stco` chunk offset pointing into the earlier `mdat` still correct; the code
  checks that layout holds and declines rather than corrupting a recording.

  `mdcv` and `clli` go in beside it. They are not decoration: handed HDR with
  no light-level metadata a player must assume the content might be graded for
  a very bright display and tone-maps defensively, which on a screen recording
  that never exceeds SDR white costs real brightness. MaxCLL and MaxFALL are
  **measured**, not guessed - the luma plane is already being copied out of the
  staging texture every frame, so the peak and the mean are taken during that
  copy while the data is still in cache. The mastering display comes from
  `DXGI_OUTPUT_DESC1`, which is the honest answer to what the content was made
  on.

  A finished HDR recording now reads: `hvcC` profile_idc 2 (Main 10), 4:2:0,
  luma and chroma 10-bit; `colr` primaries 9, transfer 16, matrix 9, limited
  range; `mdcv` with the panel's real primaries and 426 nits peak; `clli` with
  the measured MaxCLL and MaxFALL; `tkhd` and `hvc1` both the requested size.
  The HEVC VUI agrees with all of it - `video_full_range_flag` 0, primaries 9,
  transfer 16, matrix 9 - which was checked by parsing the SPS, because
  decoders believe the bitstream over the container.

- The region is **clipped to one monitor** - the one containing its centre.
  Recording a composite of several outputs would need one duplication each and a
  shared clock between them.
- Starting a recording releases the still-capture machinery, because DXGI will not
  hand a second duplication for the same output to the same process; it fails the
  second one with `E_INVALIDARG`. It is re-created when the recording ends.

## How the hotkeys work

`Win+Shift+S` belongs to the shell, which hands it to the `Microsoft.ScreenSketch`
package. `RegisterHotKey` cannot claim it while that package is installed, so
Nitshot installs a `WH_KEYBOARD_LL` hook instead. Low-level hooks run
most-recently-installed first, so ours sees the chord before the shell's does and
swallows it. **Nothing is uninstalled and nothing is permanent** — quit the program
and Snip & Sketch has its shortcut back.

Two details that matter:

- Swallowing the `S` would leave the shell thinking the Windows key was tapped alone,
  which opens the Start menu on release. The hook injects a reserved `VK_NONAME`
  keystroke so the chord counts as consumed.
- The hook does **not** filter on `LLKHF_INJECTED`. Remote-desktop hosts deliver every
  real keypress as injected, and the hotkey has to keep working over CRD/RDP. Instead,
  our own filler keystroke carries a tag in `dwExtraInfo` and only that is skipped.

`Alt+PrtScn` is deliberately left alone — that stays Windows' own window grab.

The hook cannot see keys while an elevated window has focus. That is a Windows
restriction on non-elevated hooks, not something the program can work around.

**A low-level hook is dispatched on the thread that installed it**, so that thread
must never be busy. Phase 4 briefly broke this: a 4K HDR snip spent seconds in the
encoder on the message loop, the hook could not answer within
`LowLevelHooksTimeout`, and Windows passed Win+Shift+S to the shell — which opened
Snip & Sketch, the one thing this program exists to prevent. Saving therefore runs
on a worker thread (`Save_SnipAsync`), and the hook is reinstalled every 10 seconds
as a safety net, since there is no way to ask whether it is still alive.

## Why not Windows.Graphics.Capture

WGC is what draws the yellow "your screen is being captured" border, and that border
is what sometimes gets stuck (see the sibling `capturefix` tool). Nitshot
uses DXGI Desktop Duplication, which has no such overlay.

## Building

```
.\build.bat
```

MSVC 2022 Build Tools, x64, `/W4 /WX`. Falls back to MinGW-w64 if MSVC is not present.

## Language

English and German ship; the interface follows the Windows display language by
default. Everything visible lives in `langs\` beside the exe, one file per
language, and English is *also* compiled in as a last-resort fallback so a
missing or damaged folder can never leave the window blank.

```
Nitshot.exe
langs\
  en.ini      the template - copy it to add a language
  de.ini
```

**Adding one needs no rebuild and no restart.** Copy `en.ini` to `fr.ini`,
translate the text after each `=`, and the language appears in Settings by
itself, named the way Windows names it ("Français", not "French"). Choosing it
switches the open window there and then, because being asked to accept a
language you cannot read yet is a poor way to choose one. `%APPDATA%\Nitshot\langs\`
is searched first, so a shipped translation can be replaced where the program
folder is read-only.

Four decisions worth knowing about:

- **The fallback is per string, not per file.** A key that is missing, empty or
  misspelt falls back to English on its own, so a half-finished translation is
  usable from the first line. Verified with a deliberately broken `fr.ini`:
  6 of 79 keys translated, one unknown key, one malformed line and a run of
  invalid UTF-8 — the six appeared in French, everything else in English, the
  bad bytes rendered as replacement glyphs, and nothing else was affected.
- **The files are parsed here, not by `GetPrivateProfileString`.** That API
  reads a BOM-less file in the system code page, and `settings.ini` is exactly
  that, so a translator saving UTF-8 out of Notepad would have got mojibake
  umlauts. `langs\*.ini` are UTF-8 with or without a BOM.
- **`%1`, `%2` are substituted positionally**, so a translation may reorder
  them — the German `TOAST_RECORDED` does exactly that. This is deliberately
  not `FormatMessage`, which treats `%n` as a hard line break and would let a
  user-supplied file break the formatter rather than merely look wrong.
- **Product vocabulary stays English in every language**: Nitshot,
  snip, HDR, PrtScn, Win+Shift+S, the file extensions, the codec names.
  Somebody looking for `.jxr` should find `.jxr`.

Log lines are diagnostics rather than interface and are always English.

## Settings

**Tray icon → Settings…** covers all of it. The dialog follows the Windows
light/dark setting, shows whether advanced colour is actually on and what SDR white
is currently at, and greys out the options that depend on one another rather than
letting them be set to combinations that do nothing.

It is split into five tabs — **General · HDR · Recording · Hotkeys · Advanced** —
switched by click, the arrow keys on the strip, or `Ctrl+Tab` / `Ctrl+Shift+Tab`
(`Ctrl+PgDn` / `Ctrl+PgUp`) from anywhere in the dialog, and it reopens on the tab
it was closed on. The strip is drawn by the app rather than being a stock tab
control, which has no dark mode and paints its page in a colour that does not
match the dialog.

**Apply** saves and takes effect immediately without closing, and is only enabled
while there is something to apply. **Cancel** then undoes only what changed since
the last Apply. Choosing a language switches the dialog at once so you can read
what you are accepting; Cancel switches it back.

It opens **centred on whichever monitor the pointer is on**, and if you drag it
somewhere else it opens there next time — including after Cancel, since where a
window sits is not one of the settings being edited. A remembered position is only
honoured while it still lands on a monitor; unplug that display and the dialog goes
back to being centred rather than opening somewhere you cannot reach it. Leaving it
where it is keeps it following the centre as the desktop changes.

Everything is also in `%APPDATA%\Nitshot\settings.ini`, written with
defaults on first run.

| Key | Default | Meaning |
|---|---|---|
| `CopyToClipboard` | 1 | put every snip on the clipboard |
| `SaveToDisk` | 1 | also write a file |
| `SaveFolder` | *(empty)* | empty means `Pictures\Nitshot` |
| `VideoFolder` | *(empty)* | where recordings go; empty means `Videos\Nitshot`. Recordings are always saved, so this does not depend on `SaveToDisk` |
| `HookWinShiftS` | 1 | take over `Win+Shift+S` |
| `HookPrintScreen` | 1 | take over `PrtScn` |
| `PrtScnFullscreen` | 0 | `PrtScn` grabs everything without showing the overlay |
| `DefaultMode` | 0 | 0 rectangle, 1 freeform, 2 window, 3 fullscreen |
| `DimPercent` | 40 | how far the overlay dims the desktop |
| `ShowToast` | 1 | tray balloon naming the saved file |
| `HdrSaveJxr` | 1 | HDR snips go to `.jxr` |
| `HdrSdrSidecar` | 1 | …plus a tone-mapped `.png` beside it |
| `HdrSdrRollOff` | 0 | keep highlight detail in the 8-bit copy, at the cost of white landing near 231 |
| `HdrJxrQuality` | 90 | 1–100; 100 is true lossless and much slower |
| `ForceSdr` | 0 | ignore advanced colour and capture 8-bit |
| `RecordAudio` | 1 | 0 none, 1 system sound, 2 microphone |
| `RecordFps` | 30 | 5–60 |
| `RecordRawAudio` | 1 | ask the endpoint to bypass its effects; many drivers refuse |
| `RecordEncoder` | `auto` | `auto`, `h264`, `hevc8` or `hevc10`; only what probes clean is offered |
| `RecordHdr` | 1 | capture 10-bit HDR when the display is in it |
| `RecordWavSidecar` | 0 | also write the untouched PCM as a `.wav` beside the `.mp4` |
| `RecordMuxFromWav` | 0 | with the above: no live AAC; build the `.mp4`'s sound from the `.wav` afterwards |
| `SettingsX`, `SettingsY` | *unset* | where the settings window was dragged; unset means centred |
| `Language` | `auto` | `auto` follows Windows; otherwise a tag with a `langs\<tag>.ini` |
| `ForceGdi` | 0 | skip Desktop Duplication and always use GDI |
| `DebugLog` | 0 | append diagnostics to `Nitshot.log` beside this file |

`DebugLog` exists because everything interesting here happens with no window on
screen: a hotkey that never arrived and a capture that failed look identical from
the outside. With it on, **F12 inside the overlay** writes the swap chain's back
buffer to `overlay-debug.png` in the screenshots folder — a flip-model swap chain
is invisible to every ordinary screen-capture route, `BitBlt` and `PrintWindow`
both returning the desktop underneath it, so that dump is the only way to look at
what the overlay actually rendered.

## A note on desktops

The hook, and capture, live on the `Default` desktop. When the screensaver or the
lock screen takes over, input goes to a different desktop entirely — the hotkeys
do nothing there and a GDI screen DC is invalid. That is Windows working as
designed, not a fault, and it is worth remembering while testing.

## Optional: remove the Snipping Tool

Not required — the hook works with it installed. If you would rather it were gone:

```bash
powershell.exe -NoProfile -Command "Get-AppxPackage Microsoft.ScreenSketch | Remove-AppxPackage"
```

Reinstallable from the Microsoft Store.
