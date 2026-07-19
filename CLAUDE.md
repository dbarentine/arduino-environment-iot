# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repo Is

Firmware for an **Arduino MKR WiFi 1010** (Microchip SAMD21, Cortex-M0+) that
reads environmental sensors and ships telemetry to Grafana Cloud. The board
polls I2C temperature/humidity and particulate (dust) sensors every 30s and
POSTs the readings as **OTLP/HTTP JSON** over plain HTTP to a LAN
**OpenTelemetry Collector**, which converts to protobuf and forwards to Grafana
Cloud. The device itself does no TLS and holds no cloud credentials — the
Collector owns the secure hop.

Built with **PlatformIO** on the Arduino framework. It is not an Arduino-IDE
sketch; there is no `.ino`. See `docs/FLASHING.md` for the full build/flash/
verify walkthrough (CLion + CLI).

## Branch & PR Workflow

Always begin new work from a clean, up-to-date `main`. **At the start of every
session, before the first file change, check the current branch state** — a
session often starts already checked out on a feature branch whose PR has
already merged. Run `git status -sb` (clean tree? what branch?) and, if you're
not on `main`, check whether that branch's PR is already merged
(`gh pr list --head "$(git branch --show-current)" --state all --json number,state`).
Likewise, **before the first file change of any new unit of work, if a PR was
opened earlier in the session, check whether it has merged**
(`gh pr view <#> --json state,mergedAt` or `gh pr status`). If the branch is
merged (or otherwise stale):

1. `git checkout main`
2. `git pull` (fetch + fast-forward)
3. Create a fresh branch for the new work

Never stack new edits on a stale or already-merged branch. If the PR is still
open, keep working on the current branch.

## Build, Flash & Monitor

PlatformIO Core (`pio`) drives everything; the default (and only) environment is
`mkrwifi1010`.

```bash
pio run                       # compile
pio run -t upload             # compile + flash over USB (BOSSA bootloader)
pio run -t upload --upload-port /dev/ttyACM0   # pin the port
pio device monitor -b 115200  # serial monitor (115200 baud)
pio device list               # find the board's port
pio run -t clean              # wipe .pio build output
```

Only one process can own the serial port — **close the monitor before
uploading**. If an upload times out, double-tap RESET to force bootloader mode
(the LED fades in/out), then re-upload; the port may change to a different
`/dev/ttyACMx`. On Linux you must be in the `dialout` group to access the port
(`sudo usermod -aG dialout $USER`, then log out/in). Full troubleshooting table
is in `docs/FLASHING.md`.

## Toolchain Notes

- **C++ standard:** the build forces `gnu++17` (see `[common]` in
  `platformio.ini`), not plain `-std=c++17`. The SAMD core relies on GNU
  extensions (e.g. the `ushort` typedef used throughout the sensor code), so the
  strict dialect will not compile. `build_flags`/`build_unflags` wire this into
  the `mkrwifi1010` env.
- **Library pins are deliberate** — read the inline comments in `platformio.ini`
  before bumping any version. Notably: `ArduinoHttpClient` is now a direct dep
  (it used to come in transitively via the old `Arduino_ConnectionHandler`
  0.6.x); `Arduino_ConnectionHandler` is pinned to `^0.7.7` because 0.6.5 was
  yanked from the registry. Several libs are pinned to GitHub/tarball URLs
  (`LibPrintf`, `Seeed_SHT35`, `FreeMemory`).
- **Memory is tight.** The build reports flash/RAM usage on every link (release
  currently ~77% flash, ~29% RAM of the SAMD21's 256K/32K). Watch the numbers
  when adding libraries or large stack/heap allocations. There is deliberate
  effort to keep config as compile-time constants so no heap is touched before
  `setup()`.

## Configuration & Secrets

All device config lives in **`include/arduino_secrets.h`** (tracked in the repo
with **placeholder** values — no real credentials are committed). Before
flashing to hardware, set:

- `SECRET_SSID` / `SECRET_PASS` — WiFi credentials.
- `OTEL_COLLECTOR_HOST` — the Collector's LoadBalancer IP on the LAN. This
  **must be a numeric IP, not a hostname**: `publishMessage()` connects via the
  `IPAddress` overload to skip the blocking DNS lookup (a `hostByName()` on an
  unreachable network can stall past the watchdog window). A non-numeric value
  is rejected at publish time and no request is sent.
- `OTEL_COLLECTOR_PORT` — `4318` (OTLP/HTTP).
- `OTEL_SERVICE_NAME` — becomes the OTLP `service.name`, which maps to the
  Prometheus `job` label in Grafana.

The device and the Collector must be on the same LAN/VLAN and able to reach
`HOST:PORT`. If real secrets ever need to be kept out of the repo, add
`include/arduino_secrets.h` to `.gitignore` and ship a `.example` template —
today the file is intentionally committed with placeholders.

### Sensor topology is data, not code

The same header declares the sensor inventory as two `std::map`s
(`tempHumiditySensors`, `dustSensors`). Each entry is
`{ "name", { "location", usesMultiplexer, channel, i2cAddress } }`. `setup()`
iterates these maps and registers each sensor with `SensorService`. **To add,
move, or re-address a sensor, edit these maps — no code changes needed.**
`location` and `name` become OTLP data-point attributes on every reading.

## Architecture

Three translation units under `src/`, with headers in `include/`:

- **`main.cpp`** — orchestration. Sets up WiFi (`WiFiConnectionHandler` from
  `Arduino_ConnectionHandler`), the RTC (`RTCZero`, kept in GMT), the hardware
  watchdog, and an `arduino-timer` schedule. `loop()` only feeds the watchdog,
  runs `conMan.check()`, and ticks the timer. Two scheduled jobs: `readSensors`
  every 30s and `setRTC` (NTP resync) hourly. `publishMessage()` is the HTTP
  POST callback handed to the sensor layer.
- **`SensorService.{h,cpp}`** — owns all sensor I/O and OTLP serialization.
  Supports `SHT35` temp/humidity sensors and `HM330X` particulate sensors, each
  optionally behind a **`TCA9548` I2C multiplexer** (selected/deselected per
  read by channel). `readAndPublishSensors()` reads every registered sensor,
  builds one OTLP `resourceMetrics` JSON payload (a gauge per measurement, with
  a NumberDataPoint per sensor), and invokes the publish callback. Metric names:
  `environment.temperature_fahrenheit`, `environment.humidity_percent`,
  `environment.pm1_0_ugm3`, `environment.pm2_5_ugm3`, `environment.pm10_ugm3`
  (dots become underscores in Prometheus).
- **`SerialLogger.{h,cpp}`** — `printf`-style leveled serial logging
  (`Debug/Info/Warning/Error`) via `LibPrintf`, timestamped from the RTC, plus
  `LogNetworkInformation()` for IP/MAC.

### Watchdog / hang recovery

The SAMD21 hardware watchdog (Adafruit SleepyDog, `Watchdog.enable(...)`) is
armed early in `setup()` with a ~16s window (the SAMD21 max). It is fed
(`Watchdog.reset()`) each `loop()` iteration, at the top of every read/publish
tick, and while waiting on NTP — so a genuine hang (wedged I2C read, stalled
WiFi/HTTP) reboots the board, but normal slow cycles do not. The HTTP response
timeout (`HTTP_RESPONSE_TIMEOUT_MS`, 8s) keeps the *response* wait short.

**The watchdog is deliberately suspended around the publish network I/O.**
`publishMessage()` calls `Watchdog.disable()` before the request and re-arms it
(`Watchdog.enable(WATCHDOG_TIMEOUT_MS)`) on every exit path. This is because an
unreachable Collector makes the WiFiNINA client block inside a single SPI-driver
call (TCP connect) for longer than the 16s window — longer than the response
timeout covers, and unfeddable from inside the library. Since the SAMD21 WDT
caps at ~16s we can't widen it, so we drop coverage only for the bounded network
call (connect/response/`stop()` all self-terminate) while `loop()`, the sensor
reads, WiFi and NTP stay watchdog-covered. Net effect: an unreachable Collector
logs an error and the board keeps running instead of reboot-looping.

If the serial monitor shows `Startup` every ~16s, something in **setup/connect**
(not the publish) is hanging past the window — check WiFi and NTP/DNS
reachability.

## Verifying Data End-to-End

A successful publish logs `status code of 200` every 30s in the serial monitor.
Then in Grafana → Explore (Prometheus datasource):

```promql
environment_temperature_fahrenheit{job="arduino-environment-iot"}
```

First data can take ~1 minute to appear in Grafana Cloud. If the status code is
not 200 (e.g. `-3`/timeout), the device can't reach the Collector — verify
`HOST:PORT` is reachable from the device's network.

## Layout

```
platformio.ini    # env, board, C++ dialect, pinned lib_deps (read the comments)
include/          # headers + arduino_secrets.h (config + sensor inventory)
src/              # main.cpp, SensorService.cpp, SerialLogger.cpp
docs/FLASHING.md  # full CLion + CLI build/flash/verify + troubleshooting
lib/  test/       # PlatformIO conventions (currently just READMEs)
CMakeLists*.txt   # generated by `pio project init --ide clion` — do not hand-edit
```
