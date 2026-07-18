# Flashing arduino-environment-iot (CLion + PlatformIO)

The target board is an **Arduino MKR WiFi 1010** (SAMD21). It flashes over USB
via the BOSSA bootloader; PlatformIO drives the build and upload. These
instructions assume **JetBrains CLion** as the IDE, with CLI equivalents for
every step.

The firmware sends OTLP/JSON telemetry over plain HTTP to the LAN OpenTelemetry
Collector, which forwards it to Grafana Cloud. See the top-level `README`/repo
history for the architecture.

---

## 1. One-time CLion + PlatformIO setup

1. **PlatformIO Core** must be installed (the engine CLion drives). Verify:
   ```bash
   pio --version
   ```
   If missing: `pipx install platformio` (or `pip install --user platformio`).
2. **CLion plugin:** Settings → Plugins → Marketplace → search
   **"PlatformIO for CLion"** → Install → restart CLion.
3. **Generate CLion project files** (code insight + run configs):
   ```bash
   pio project init --ide clion
   ```
4. **Open the project folder** in CLion (`File → Open` → repo root). CLion reads
   `platformio.ini` and adds PlatformIO **Build / Upload / Monitor** run
   configurations targeting the `mkrwifi1010` environment.

## 2. Confirm config before flashing

Open `include/arduino_secrets.h` and check:

- `SECRET_SSID` / `SECRET_PASS` — your WiFi.
- `OTEL_COLLECTOR_HOST` — the Collector's LoadBalancer IP, **`"10.10.4.234"`**.
- `OTEL_COLLECTOR_PORT` — `4318`.

> The device and the Collector must be on the same LAN/VLAN as `10.10.4.234`
> (MetalLB L2). If the node sits on an isolated IoT VLAN, make sure it can reach
> that `IP:port`.

## 3. Connect the board & find its port

Plug the MKR into USB with a **data-capable** cable (not charge-only), then:

```bash
pio device list
```

- **Linux:** `/dev/ttyACM0` — you must be in the `dialout` group:
  `sudo usermod -aG dialout $USER`, then log out/in.
- **macOS:** `/dev/cu.usbmodemXXXX`
- **Windows:** `COMx`

PlatformIO usually auto-detects the port, so you rarely need to set it.

## 4. Build & upload (flash)

**CLion:** select the **Upload** run configuration (or PlatformIO tool window →
*Project Tasks → mkrwifi1010 → General → Upload*) and click ▶. It compiles, then
flashes.

**CLI equivalent:**

```bash
pio run -t upload
# or pin the port: pio run -t upload --upload-port /dev/ttyACM0
```

A successful flash ends with `[SUCCESS]` and the board reboots into the new
firmware.

## 5. Watch the serial output (verification)

**CLion:** the **Monitor** run config, or *PlatformIO → Serial Monitor*.
**CLI:** `pio device monitor -b 115200`.

Expected sequence:

```
... Startup
... Watchdog enabled (16000 ms)          <- watchdog active
... IP Address: ...   MAC address: ...    <- WiFi connected
... Updating RTC from NTP server with Epoch of: ...
```

then every 30 s:

```
... Posting OTLP metrics to http://10.10.4.234:4318/v1/metrics
... Finished reading and publishing sensors with a status code of 200
```

**`200` = success.** Only one process can own the serial port — close the
monitor before re-uploading.

## 6. Confirm data landed

In Grafana → **Explore** on the Prometheus datasource:

```promql
environment_temperature_fahrenheit{job="arduino-environment-iot"}
```

Then open the **Environment Data** dashboard — panels should populate. First
data may take ~1 minute to appear in Grafana Cloud.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Upload fails / "port busy" | Close the Serial Monitor first — it holds the port. |
| Upload times out / board won't flash | **Double-tap the RESET button** quickly; the onboard LED fades in/out (bootloader mode), then upload again. The port may change to a different `/dev/ttyACMx` in bootloader — re-run `pio device list`. |
| Permission denied on the port (Linux) | Add yourself to the `dialout` group (step 3). |
| Wrong port auto-picked | Add `upload_port = /dev/ttyACM0` (and `monitor_port = …`) under `[env:mkrwifi1010]` in `platformio.ini`. |
| Repeated `Startup` every ~16 s in the monitor | The watchdog is resetting because something in `setup()`/connect hangs > 16 s — usually WiFi association or NTP. Check credentials and that NTP/DNS is reachable on that network. |
| `status code` is not `200` (e.g. `-3`/timeout) | The device can't reach the Collector. Confirm `10.10.4.234:4318` is reachable from the device's network (`kubectl get svc otel-collector -o wide` to check the IP is still assigned). |
