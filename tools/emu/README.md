# End-to-end testing of Switch homebrew on an emulator

Scripts to run a `.nro` in a headless Switch emulator, send it a sequence of
button presses, capture the screen, and assert on what the program actually
wrote to the SD card. Nothing here is Torfoil specific except the test scenario:
the runner takes any `.nro`.

```powershell
powershell -File tools\emu\Install-TorfoilEmu.ps1 -Firmware "...\Firmware 22.1.0.zip" -Keys "...\keys"
powershell -File tools\emu\Invoke-TorfoilEmu.ps1 -Script 'wait 4000,shot,A!,shot,quit'
powershell -File tools\emu\Test-TorfoilEmu.ps1
```

The full test takes about a minute and exits 0 or 1, so it drops into CI as is.

## Why this is verifiable

The emulator's SD card is a plain host directory. The guest writes its logs,
settings and files there, and the test reads them back from the host afterwards.
Assertions are therefore about real effects, not pixels:

| Assertion | What it proves |
|---|---|
| `torfoil.log` exists | the app booted and can write to the card |
| 5x "stockage prêt" | the five `.torrent` files dropped in `inbox` were read, mounted and verified |
| `inbox` is empty | import deletes what it accepted |
| 5 resume files | the engine persisted its resume state |
| no "ÉCHEC" line | no SD write failed |
| screen changes after **A** | keyboard input reaches the guest |
| no uniform screenshot | frames are really rendered, not a black screen |

## Emulator

[Kenji-NX](https://git.ryujinx.app/projects/Kenji-NX), a maintained Ryujinx
fork, in `--no-gui` mode. That is its headless mode: it opens a render window
but no emulator UI, and everything is driven from the command line.

Firmware and keys come from **your own** console. They are neither bundled nor
downloaded. `install_firmware.py` reproduces what the emulator's GUI installer
does (each `.nca` becomes a directory `<id>.nca` holding a file named `00`),
because that feature has no command line equivalent.

## Three pitfalls and their fixes

**1. Keyboard layout.** The emulator maps Switch buttons to *physical* key
positions. Sending a virtual key (`keybd_event` with a VK code) lands on a
different physical key depending on the layout: on AZERTY, "Z" is not where the
emulator expects it, so the A button never fires. The fix has two parts:
`SendInput` with hardware **scancodes** (`KEYEVENTF_SCANCODE`), and a dedicated
input profile (`profile-torfoil.json`) that only uses keys which sit in the same
place on every layout (arrows, Space, Backspace, Insert, Delete, Home, End,
PageUp, PageDown, Enter, Tab). Avoid F1 to F12: the emulator binds those to its
own hotkeys.

**2. Dropped presses.** The guest samples buttons once per frame. When the
emulator is busy (verifying torrents, compiling shaders), a short press falls
between two frames and vanishes. Two mitigations: keys are held for 700 ms, and
the `!` suffix means **insist**, that is, wait for the frame to settle, press,
check that the screen changed, and retry if it did not. An `!` key with no
effect after six attempts fails the run, so the harness cannot silently report
success for navigation that never happened.

**3. Screenshots.** `CopyFromScreen` grabs a screen region, so any notification
popping over the window ends up in the image. `PrintWindow` with
`PW_RENDERFULLCONTENT` asks the window for its own contents and captures only
that.

## Script actions

`A B X Y L R ZL ZR + -`, `Haut Bas Gauche Droite`, `wait <ms>`, `shot`, `quit`

Append `!` to a key for insist mode. Use it for any navigation that later steps
depend on (opening a menu, switching tabs). It is pointless for keys whose
effect is not visible.

## Linux variant, no GPU

`setup.sh`, `run.sh` and `e2e.sh` do the same thing on Linux (including WSL):
Xvfb, llvmpipe software rendering, `xdotool`, `ffmpeg` captures. No GPU is
required, which is what makes it CI friendly.

Two emulator patches are needed, both in `patches/`:

- `0001-headless-swap-interval`: without it the emulator dies at the first
  frame. It treats a failing `SDL_GL_SetSwapInterval` as fatal, and a virtual X
  server offers no swap control extension.
- `0002-bufferqueue-timed-wait`: the wait for a free display buffer uses a
  monitor with no timeout, so a missed wakeup hangs the guest forever. Upstream
  even carries a `TODO` about that condvar.

**Known unresolved limitation**: even patched, the guest freezes after a few
frames under pure software rendering, waiting on a graphics resource the
emulator never returns. The Linux variant therefore checks boot, the first
screen and SD writes, but not navigation. For full UI testing, use the Windows
variant with a real GPU.
