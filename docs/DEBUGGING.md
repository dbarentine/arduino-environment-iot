# Debugging arduino-environment-iot (Atmel-ICE + SWD)

The MKR WiFi 1010 (SAMD21, Cortex-M0+) has **no on-board debugger** — the USB
port is the SAMD21's native USB, used for BOSSA flashing and the serial monitor,
not for debug. On-chip debugging (breakpoints, single-step, memory/register
inspection) needs an **external SWD probe**. This guide covers the **Atmel-ICE**.

Normal development does not need any of this — flash over USB and read the serial
monitor (`docs/FLASHING.md`). Reach for SWD when serial `printf` isn't enough:
hard faults, hangs you can't localize, or inspecting live state.

> **The `pio debug` KeyError.** Running `pio debug` against the default
> `mkrwifi1010` env crashes with `KeyError: 'tool-openocd'`. That env pins
> `build_type = release`; the atmelsam platform only keeps the OpenOCD package
> when `build_type == debug`, so on the release env it deletes the package and
> the debug-server lookup then throws. The fix is to debug the dedicated
> **`mkrwifi1010_debug`** env (added to `platformio.ini`), which sets
> `build_type = debug`. Don't debug the release env.

---

## 1. What you need

- **Atmel-ICE** (or Atmel-ICE Basic) with its **50-mil 10-pin SAM cable**
  (the ARM 10-pin cable + the "AVR" cable will *not* fit the SAM port pinout).
- The MKR WiFi 1010, powered over its **USB port** (the probe does not power the
  target — keep USB plugged in for both power and the serial monitor).
- PlatformIO Core (already required for this repo). The OpenOCD package
  (`tool-openocd`) installs automatically the first time you debug.
- **Linux udev rules** so a non-root user can reach the probe (see §5).

---

## 2. Wiring: Atmel-ICE SAM port → MKR WiFi 1010 SWD

The Atmel-ICE **SAM** port is a 2x5 50-mil header. For SWD only five signals
matter; SAMD21 (M0+) has no SWO/trace, and TDI/TDO are JTAG-only, so leave them
unconnected.

| Atmel-ICE SAM pin | Signal        | MKR WiFi 1010            |
| ----------------- | ------------- | ------------------------ |
| 1                 | VTG (Vref)    | **3V3** (voltage sense)  |
| 2                 | SWDIO         | **SWDIO** pad            |
| 3                 | GND           | **GND**                  |
| 4                 | SWCLK         | **SWCLK** pad            |
| 5                 | GND           | GND (optional 2nd gnd)   |
| 6                 | SWO           | — (not present on M0+)   |
| 8                 | nTRST/TDI     | — (JTAG only, unused)    |
| 10                | nRESET        | **RESET** (recommended)  |

Notes:

- **VTG must be connected.** The Atmel-ICE senses target voltage on pin 1 and
  won't drive the SWD lines without it. Tie it to the board's **3V3**, not 5V.
- The MKR WiFi 1010 exposes **SWDIO** and **SWCLK** on labeled pads (the SWD
  test points, not the numbered Arduino headers). Identify them on the official
  Arduino MKR WiFi 1010 pinout/schematic before wiring — verify with a
  continuity/voltage check rather than trusting a photo.
- **nRESET is optional but recommended.** Without it OpenOCD relies on SWD
  connect-under-reset, which usually works on the SAMD21 but is less reliable if
  firmware reconfigures the SWD pins or sleeps aggressively.
- Keep the leads short. SWD is happy at the platform's default speed; long
  jumpers can cause intermittent "target not halted" errors.

---

## 3. Config (already in `platformio.ini`)

A separate env isolates debugging from the tuned release build:

```ini
[env:mkrwifi1010_debug]
extends = env:mkrwifi1010
build_type = debug           ; -Og -g3; also keeps tool-openocd available
debug_tool = atmel-ice       ; OpenOCD via CMSIS-DAP (the Atmel-ICE protocol)
upload_protocol = atmel-ice  ; flash over SWD too, so probe + flash stay in sync
debug_init_break = tbreak setup
```

- **`extends`** inherits every lib, build flag, and the `gnu++17` dialect from
  `env:mkrwifi1010`; only the debug-specific keys are overridden.
- **`debug_init_break = tbreak setup`** halts at the top of `setup()` when the
  session attaches. Change to `tbreak loop` or remove it (`= ` empty) to let the
  program free-run until you hit ▮ Pause or a set breakpoint.

> **Memory.** Debug builds (`-Og -g3`) are larger than release. The release
> image is already ~77% flash / ~29% RAM; watch the usage line printed on link.
> If a debug build overflows flash, drop `-g3` toward `-g1` via a
> `debug_build_flags` override, or debug with fewer libs.

---

## 4. Debug from the CLI

```bash
# Build the debug image and flash it over SWD (Atmel-ICE):
pio run -e mkrwifi1010_debug -t upload

# Start an interactive gdb session (auto-launches OpenOCD as the gdb server):
pio debug -e mkrwifi1010_debug --interface=gdb
```

The first run downloads `tool-openocd`. At the `(gdb)` prompt: `continue`,
`next`, `step`, `break SensorService.cpp:NN`, `print <var>`, `monitor reset halt`.
`quit` ends the session.

To sanity-check the probe/wiring without a full session:

```bash
pio debug -e mkrwifi1010_debug --interface=gdb -x /dev/null
```

A clean OpenOCD banner ending in `... hardware has N breakpoints, M watchpoints`
and a halted target means the SWD link is good.

---

## 5. Debug from CLion

1. Regenerate IDE files so the new env appears:
   ```bash
   pio project init --ide clion
   ```
2. CLion's PlatformIO plugin adds a **PIO Debug (mkrwifi1010_debug)** run
   configuration. Select it and click the **Debug** (🐞) button — it builds,
   flashes over the Atmel-ICE, launches OpenOCD + gdb, and stops at the
   `debug_init_break`.
3. Set breakpoints in the gutter; use the Variables/Registers/Memory views and
   Step Over/Into as usual. The **Peripherals** view is populated from the
   board's SVD (`ATSAMD21G18A.svd`) — handy for inspecting WDT/SERCOM/RTC.

> The serial monitor and an active debug session can share the USB cable (USB is
> power + CDC serial; SWD is the separate probe), so you can watch `Logger`
> output and step at the same time. Only **uploads** contend for the port — and
> in the debug env uploads go over SWD, not USB.

---

## 6. The watchdog and breakpoints — no action needed

This firmware arms the SAMD21 hardware watchdog early in `setup()`
(`Watchdog.enable(...)`, ~16s window; see `src/main.cpp` and the watchdog notes
in `CLAUDE.md`). A reasonable fear is that pausing at a breakpoint for >16s would
trip a reset mid-session. **It won't.**

On the SAMD21 the WDT halts whenever the core is halted in debug mode, because
`WDT->DBGCTRL.DBGRUN` defaults to `0` and Adafruit SleepyDog never changes it.
So while you sit at a breakpoint the watchdog clock is stopped; it resumes when
you `continue`. Normal `Watchdog.reset()` feeding in `loop()` covers the running
case as always.

**If** you ever do see the board resetting roughly every ~16s *while halted*
(e.g. running a debugger that leaves peripherals free-running, or after changing
`DBGRUN`), guard the watchdog out of debug builds. Add a define in the debug env:

```ini
[env:mkrwifi1010_debug]
build_flags = ${common.build_flags} -DDISABLE_WATCHDOG
```

and gate the two watchdog touchpoints in `src/main.cpp`:

```cpp
#ifndef DISABLE_WATCHDOG
    int wdtMs = Watchdog.enable(WATCHDOG_TIMEOUT_MS);
    Logger.Info("Watchdog enabled (%d ms)", wdtMs);
#endif
```

(with the matching `Watchdog.reset()` calls also `#ifndef`'d). This is a
fallback, not a required step — leave the watchdog on unless you actually hit the
problem.

---

## 7. Troubleshooting

| Symptom | Likely cause / fix |
| --- | --- |
| `KeyError: 'tool-openocd'` | You ran `pio debug` on the release env. Use `-e mkrwifi1010_debug` (see the note at the top). |
| OpenOCD: `Error: init mode failed (unable to connect to the target)` | VTG (pin 1) not tied to 3V3, or SWDIO/SWCLK swapped/loose. Recheck §2; connect nRESET. |
| `Error: SWD... could not halt target` intermittently | Long/noisy leads, or firmware reconfigured SWD pins. Shorten leads, wire nRESET, retry `monitor reset halt`. |
| Linux: probe not found / permission denied | Install udev rules: `pio debug` prints the command, or run `curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \| sudo tee /etc/udev/rules.d/99-platformio-udev.rules`, then `sudo udevadm control --reload-rules && sudo udevadm trigger`, and replug. Be in the `plugdev` group. |
| Board won't flash over USB after debugging | SWD upload leaves no bootloader magic; double-tap RESET to force the BOSSA bootloader, then `pio run -t upload` (default env) over USB as usual. |
| Debug image overflows flash | Lower debug symbol level (`-g1`) or reduce libs (see §3 memory note). |

---

## 8. Quick reference

```bash
pio run -e mkrwifi1010_debug -t upload                 # flash debug build over SWD
pio debug -e mkrwifi1010_debug --interface=gdb         # interactive gdb + OpenOCD
pio run -t upload                                       # back to normal USB flashing (release)
pio device monitor -b 115200                            # serial (works alongside a debug session)
```

See `docs/FLASHING.md` for the normal USB build/flash/verify workflow.
