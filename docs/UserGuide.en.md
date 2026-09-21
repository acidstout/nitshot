# Nitshot — User Guide

Version 0.8 · © 2026 Rekow IT

Nitshot is a fast replacement for the Windows Snipping Tool. Press
**Win+Shift+S** or **PrtScn**, pick what you want, and the screenshot is on the
clipboard and saved as a file at the same moment. It can also record an area of
the screen as a video, and on an HDR display it keeps the full brightness range
instead of flattening it.

- [Getting started](#getting-started)
- [Taking a snip](#taking-a-snip)
- [Where your snips go](#where-your-snips-go)
- [HDR screenshots](#hdr-screenshots)
- [Recording the screen](#recording-the-screen)
- [The tray icon](#the-tray-icon)
- [Settings](#settings)
- [Languages](#languages)
- [Troubleshooting](#troubleshooting)

---

## Getting started

**Requirements:** Windows 10 or Windows 11, 64-bit.

Start `Nitshot.exe`. No window opens; the program lives in the
notification area (the tray, bottom right) as a small icon. From then on:

| Keys | What happens |
|---|---|
| **Win+Shift+S** | opens the capture overlay |
| **PrtScn** | opens the capture overlay (or captures the whole screen at once, if you set that up) |
| **Alt+PrtScn** | unchanged — still Windows' own "copy the active window" |

The Windows Snipping Tool stays installed. While Nitshot is running it
takes over these keys; quit it, and the shortcuts go straight back to Windows.
Nothing is changed permanently.

To have it start with Windows, tick **Start with Windows** in the tray menu or in
the settings.

---

## Taking a snip

Press **Win+Shift+S**. The screen freezes, dims, and a toolbar appears at the top
centre:

| Button | Key | Mode |
|---|---|---|
| Rectangle | `1` | drag a rectangle; the snip is taken when you let go |
| Freeform | `2` | draw any shape; everything outside the outline becomes transparent |
| Window | `3` | point at a window — it is highlighted — and click to take it |
| Fullscreen | `4` | takes all screens immediately |
| Record | | drag an area to **record a video** instead — see [Recording](#recording-the-screen) |
| ✕ | `Esc` | cancel |

A **right-click** also cancels.

The overlay remembers the mode you used last and opens in it next time.
Fullscreen is the exception: it is a one-off action, so the next snip opens in
your previous mode again.

Because the screen is frozen the moment you press the key, you can snip menus,
tooltips and anything else that would disappear once you start moving the mouse.

With several monitors, the overlay covers all of them, and you can drag a
selection across monitor boundaries.

---

## Where your snips go

By default every snip is

- **copied to the clipboard** — paste it straight into a mail, chat or document;
- **saved as a PNG** in `Pictures\Nitshot`, named
  `Screenshot 2026-09-21 143012.png` (date and time of the snip).

A notification in the corner tells you where the file went. Clicking
**Open screenshots folder** in the tray menu takes you there.

Freeform snips keep their transparent surroundings in the PNG file. Programs that
cannot handle transparency get the shape on a white background instead of a black
one.

You can switch off either destination, choose a different folder, and turn off the
notification in the [settings](#general).

---

## HDR screenshots

If a display has **HDR** (Windows calls it "advanced colour") switched on,
Nitshot notices this by itself and captures in full HDR:

- the snip is saved as a **`.jxr`** file (JPEG XR — the same format the Xbox Game
  Bar uses; the Windows Photos app opens it) with all highlights intact;
- next to it goes a normal **`.png`** copy for everything that does not
  understand HDR;
- the **clipboard** always gets a normal (SDR) picture, because Windows has no
  HDR clipboard format.

The regular 8-bit copy has to make a choice: bright highlights either become plain
white (the default — normal content looks exactly as on screen), or they are
compressed so that some detail stays visible, at the cost of white becoming very
slightly grey. That is the **Keep highlight details in the 8-bit copy** option.

Without an HDR display none of this applies, and snips are ordinary PNG files.

---

## Recording the screen

1. Press **Win+Shift+S** and click the **Record** button in the toolbar.
2. Drag the area you want to record. Recording starts immediately.
3. A red frame marks the recorded area; a small bar shows the running time and a
   **Stop** button. Both sit just outside the recorded area, so they are not in
   the video, and you can keep working inside the area as usual.
4. Stop with the **Stop** button, or press **Win+Shift+S** or **PrtScn** again —
   while a recording runs, the hotkey means "stop".

The video is saved as **MP4** in `Videos\Nitshot`, named
`Recording 2026-09-21 143012.mp4`, and a notification says so.

**Sound:** either what your PC is playing (*System sound*), the *Microphone*, or
nothing. One source at a time.

**HDR:** on an HDR display, recordings are made in 10-bit HDR as well, so colours
and highlights come out as they looked on screen. Your graphics card must be able
to encode HEVC (H.265) 10-bit; almost every current card can. If yours cannot, or
you switch the option off, the recording is made in normal SDR — correctly
converted, not washed out.

**Better sound:** the MP4's sound is compressed (AAC). If you need the exact
original, switch on the lossless **.wav** option: a `.wav` with the untouched sound
is saved beside the video. See [Recording settings](#recording).

---

## The tray icon

**Double-click** the icon to start a new snip. **Right-click** it for the menu:

| Entry | |
|---|---|
| New snip | same as Win+Shift+S |
| Capture whole screen | takes all screens at once, without the overlay |
| Open screenshots folder | opens the folder in Explorer |
| Start with Windows | start automatically when you sign in |
| Settings… | opens the settings |
| About | version and short help |
| Exit | quits the program; Win+Shift+S goes back to Windows |

---

## Settings

Open them with **tray icon → Settings…**. The window has five tabs. Switch with the
mouse, or with **Ctrl+Tab** / **Ctrl+Shift+Tab** from anywhere in the window.

The three buttons at the bottom:

- **OK** saves everything and closes the window.
- **Cancel** closes without saving the changes you made since the window opened —
  or since you last pressed Apply.
- **Apply** saves and activates your changes straight away but keeps the window
  open. It is only clickable when something has changed.

Options that only make sense together with another one are greyed out while that
other one is off.

### General

| Option | |
|---|---|
| Copy every snip to the clipboard | puts every snip on the clipboard |
| Save files | also saves every snip as a file |
| Pictures | folder for snips. **Browse…** lets you pick one. Empty means `Pictures\Nitshot` |
| Videos | folder for recordings. Empty means `Videos\Nitshot`. Recordings are always saved, even with *Save files* off |
| Dim the desktop by … percent | how dark the screen gets behind the overlay (0 = not at all) |
| Show a notification when a snip is saved | the message in the corner after each snip |
| Language | *Automatic* follows the Windows display language. A new choice switches the window at once, so you can read what you are choosing |
| Start with Windows | start automatically when you sign in |

### HDR

The top line tells you whether any display is currently in HDR and how bright
Windows shows normal (SDR) white on it.

| Option | |
|---|---|
| Save HDR snips as .jxr | save HDR snips as real HDR files |
| …and a tone-mapped .png beside it | also save a normal PNG copy |
| Keep highlight details in the 8-bit copy | compress bright areas in the normal copy instead of cutting them off at white |
| Quality | JPEG XR quality, 1–100. **90** is visually identical and fast. **100** is truly lossless but can take around 20 seconds and three times the space for a full 4K screen |
| Record in HDR when the display is in HDR | record 10-bit HDR video on an HDR display |
| Ignore HDR, capture 8-bit | treat every display as a normal one, for snips and recordings |

### Recording

| Option | |
|---|---|
| Sound | *None*, *System sound* (what the PC plays) or *Microphone* |
| Frames per second | 5 to 60; 30 is a good default |
| Encoder | which video encoder to use. *Automatic* picks the best one your graphics card offers. Only encoders that were tested successfully on your PC are listed; if the chosen one fails, the next best is used |
| Save the sound as a lossless .wav beside the .mp4 | keeps an exact copy of the sound as a `.wav` file |
| …and build the .mp4's sound from it, keeping only the .mp4 | the MP4's sound is made from the `.wav` after recording, and the `.wav` is then deleted. Leaves you with one file |

### Hotkeys

| Option | |
|---|---|
| Take over Win+Shift+S | Win+Shift+S opens Nitshot instead of the Windows Snipping Tool |
| Take over PrtScn | the same for the PrtScn key |
| PrtScn takes the whole screen without the overlay | PrtScn saves all screens immediately, no selection |

### Advanced

| Option | |
|---|---|
| Always use GDI capture | an older, slower capture method. Only useful if snips come out black or wrong on unusual display setups |
| Diagnostic log | writes a log file for troubleshooting (see below) |

The settings window opens in the middle of the screen. If you move it, it opens in
that place next time.

---

## Languages

English and German are included. By default the program uses the Windows display
language, and English where no translation exists.

**Adding your own language** needs no programming:

1. Open the `langs` folder next to `Nitshot.exe`.
2. Copy `en.ini` and name the copy after the language code, e.g. `fr.ini`.
3. Translate the text after each `=` and save the file as UTF-8 (Notepad's
   default).
4. Open the settings — the new language is already in the list.

Lines you have not translated yet simply stay English, so a half-finished
translation already works. Placeholders such as `%1` and `%2` must be kept, but
may be moved around in the sentence. Words like *snip*, *HDR*, *PrtScn* and file
extensions stay as they are in every language.

If the program folder is write-protected, put the file in
`%APPDATA%\Nitshot\langs\` instead.

---

## Troubleshooting

**Win+Shift+S opens the Windows Snipping Tool.**
Check that Nitshot is running (tray icon) and that *Take over
Win+Shift+S* is ticked. While a program that runs **as administrator** has the
focus, Windows does not let ordinary programs see the keyboard — click on a normal
window first, or use the tray icon.

**Nothing happens on the lock screen or during the screensaver.**
That is intended: Windows keeps those screens separate from the desktop.

**An HDR video looks pale or greyish in the player.**
The file is fine; the player is converting it badly. In MPC-HC / MPC-BE with the
MPC Video Renderer, turn off *Convert HDR to SDR* or set the display brightness to
your monitor's real peak brightness.

**A recording sounds a little different from the original.**
With *System sound*, the recording contains what Windows sends to your speakers —
including any sound effects or equaliser the audio driver applies. Playing that
recording through the same effects applies them twice. Switch off "audio
enhancements" for your playback device in the Windows sound settings if you want a
neutral recording.

**A snip comes out black.**
Try *Always use GDI capture* under Advanced. This mainly matters on remote
desktops, virtual displays and unusual adapters.

**"Nothing was saved – check the settings."**
Both *Copy every snip to the clipboard* and *Save files* are switched off.

**Reporting a problem.**
Switch on *Diagnostic log* under Advanced, reproduce the problem, and send the file
`%APPDATA%\Nitshot\Nitshot.log`. It contains technical
details only, no pictures.

**Where are the settings stored?**
In `%APPDATA%\Nitshot\settings.ini`. Deleting it resets everything to
the defaults.
