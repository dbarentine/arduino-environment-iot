#include "main.h"
#include <RTCZero.h>
#include <Wire.h>
#include <arduino-timer.h>
#include <ArduinoHttpClient.h>
#include <Adafruit_SleepyDog.h>

#include "SensorService.h"
#include "SerialLogger.h"

// Hardware watchdog: the SAMD21 WDT resets the board if it is not fed within this
// window, recovering the device from hangs (a stalled WiFi/HTTP call or a wedged
// I2C sensor read). ~16s is the SAMD21 maximum; Watchdog.enable() returns the
// actual period it selected. HTTP_RESPONSE_TIMEOUT_MS is kept comfortably below
// the WDT window so a merely-slow publish fails gracefully instead of tripping a
// reset. The dog is fed each loop iteration, at the start of every read/publish
// tick, and while waiting on NTP.
static const int WATCHDOG_TIMEOUT_MS = 16000;
static const uint32_t HTTP_RESPONSE_TIMEOUT_MS = 8000;

// Definitions for the globals declared extern in main.h. Telemetry is sent as
// OTLP/HTTP JSON to a local OpenTelemetry Collector on the LAN (plain HTTP); the
// Collector forwards to Grafana Cloud. All values are compile-time constants so
// no runtime heap allocation happens before setup().
const char SSID[] = SECRET_SSID;
const char PASS[] = SECRET_PASS;
const char OTEL_HOST[] = OTEL_COLLECTOR_HOST;
const int OTEL_PORT = OTEL_COLLECTOR_PORT;
const char OTEL_METRICS_PATH[] = "/v1/metrics";
const char OTEL_SVC_NAME[] = OTEL_SERVICE_NAME;
int status = WL_IDLE_STATUS;

WiFiConnectionHandler conMan(SSID, PASS);
RTCZero rtc;
SerialLogger Logger;

auto timer = timer_create_default();

// Plain HTTP to the LAN Collector — no TLS is needed on-device (the Collector
// performs the TLS hop to Grafana Cloud).
WiFiClient wiFiClient;

SensorService sensors(Logger, true);

void setup() {
    Serial.begin(115200);
    delay(5000);

    Logger.Info("Startup");

    // Enable the watchdog early so a hang during sensor init also recovers.
    int wdtMs = Watchdog.enable(WATCHDOG_TIMEOUT_MS);
    Logger.Info("Watchdog enabled (%d ms)", wdtMs);

    rtc.begin();

    for (const auto &tempSensor: tempHumiditySensors) {
        auto[location, usesMultiplexer, channel, address] = tempSensor.second;
        Logger.Debug("Adding sensor %s for location %s", tempSensor.first, location);

        if(usesMultiplexer) {
            sensors.addTemperatureHumiditySensor(tempSensor.first, location, channel, address);
        }
        else {
            sensors.addTemperatureHumiditySensor(tempSensor.first, location, address);
        }
    }

    for (const auto &dustSensor: dustSensors) {
        auto[location, usesMultiplexer, channel, address] = dustSensor.second;
        Logger.Debug("Adding sensor %s for location %s", dustSensor.first, location);

        if(usesMultiplexer) {
            sensors.addDustSensor(dustSensor.first, location, channel, address);
        }
        else {
            sensors.addDustSensor(dustSensor.first, location, address);
        }
    }

    if (!sensors.InitializeSensors()) {
        Logger.Error("Sensor initialization failed");
    }

    setDebugMessageLevel(DBG_INFO);
    conMan.addCallback(NetworkConnectionEvent::CONNECTED, onNetworkConnect);

    timer.every(1000 * 30, readSensors);
    timer.every(1000 * 60 * 60, setRTC);
}

void loop() {
    Watchdog.reset();
    conMan.check();
    timer.tick();
}

bool readSensors(void *argument) {
    // Feed the dog right before the (blocking) sensor read + publish so a normal,
    // slightly slow cycle never trips it; a true hang inside still will.
    Watchdog.reset();
    if (WiFi.status() != WL_CONNECTED) {
        Logger.Info("Waiting on WiFi connection");
    }
    else {
        int statusCode = sensors.readAndPublishSensors(&publishMessage, OTEL_SVC_NAME);
        Logger.Debug("Finished reading and publishing sensors with a status code of %d", statusCode);
    }
    return true;
}

int publishMessage(const std::string& str) {
    // Plain HTTP to the LAN OpenTelemetry Collector; it converts to protobuf and
    // forwards to Grafana Cloud, so the device needs no TLS or credentials here.
    //
    // Connect by numeric IP, not hostname: passing OTEL_HOST as a string makes
    // WiFiClient::connect() call the blocking WiFi.hostByName() DNS lookup first,
    // which on a mis-configured/unreachable network can add several seconds on
    // top of the ~10s TCP connect -- together enough to blow past the 16s
    // watchdog and reboot the board every publish cycle. OTEL_COLLECTOR_HOST is
    // documented to be the collector's LoadBalancer IP, so parse it and use the
    // IPAddress overload to skip DNS entirely. (A non-numeric host is rejected
    // here rather than silently reintroducing the DNS stall.)
    IPAddress collectorIp;
    if (!collectorIp.fromString(OTEL_HOST)) {
        Logger.Error("OTEL_HOST '%s' is not a numeric IP; skipping publish", OTEL_HOST);
        return HTTP_ERROR_CONNECTION_FAILED;
    }

    HttpClient httpClient(wiFiClient, collectorIp, OTEL_PORT);
    // Bound the response wait below the watchdog window so a stalled collector
    // returns an error here instead of tripping a watchdog reset.
    httpClient.setHttpResponseTimeout(HTTP_RESPONSE_TIMEOUT_MS);

    const char* payload = str.c_str();
    size_t payloadLength = str.length();

    Logger.Debug("Posting OTLP metrics to http://%s:%d%s", OTEL_HOST, OTEL_PORT, OTEL_METRICS_PATH);
    //Logger.Debug("Payload %s", payload);

    // Suspend the watchdog for the network I/O below. When the collector is
    // unreachable, the WiFiNINA client blocks inside a single SPI-driver call
    // (connect, and to a lesser extent the response read/close) for longer than
    // the 16s SAMD21 watchdog window -- and we cannot feed the watchdog from
    // inside those library calls. The SAMD21 WDT maxes out at ~16s, so we can't
    // just widen the window. Instead, disable it for the bounded publish and
    // re-arm it on every exit path. The NINA stack always returns (connect has
    // its own ~10s timeout, the response wait is bounded by
    // setHttpResponseTimeout, and stop() caps at ~5s), so this window cannot
    // hang forever; loop(), the sensor reads, WiFi and NTP all remain covered by
    // the 16s hardware watchdog. See docs and the watchdog notes in CLAUDE.md.
    Watchdog.disable();

    httpClient.beginRequest();
    // post() performs the TCP connect. If the collector is unreachable (e.g. the
    // device is on the wrong VLAN/SSID) post() returns an error; bail out rather
    // than falling through to responseStatusCode() on a dead connection.
    int requestErr = httpClient.post(OTEL_METRICS_PATH);
    if (requestErr != HTTP_SUCCESS) {
        Logger.Error("Collector connect failed (%d); skipping publish this cycle", requestErr);
        httpClient.stop();
        Watchdog.enable(WATCHDOG_TIMEOUT_MS);
        return requestErr;
    }
    // HttpClient only emits a Host header when it was constructed with a
    // hostname; we use the IPAddress overload (to skip the blocking DNS lookup),
    // so it won't send one. HTTP/1.1 requires Host -- without it the collector
    // rejects the request with 400 Bad Request (empty body). Send it ourselves,
    // including the non-default port, matching what the hostname path would emit.
    char hostHeader[32];
    snprintf(hostHeader, sizeof(hostHeader), "%s:%d", OTEL_HOST, OTEL_PORT);
    httpClient.sendHeader("Host", hostHeader);
    httpClient.sendHeader("Content-Type", "application/json");
    httpClient.sendHeader(HTTP_HEADER_CONTENT_LENGTH, payloadLength);
    httpClient.beginBody();
    httpClient.write((const uint8_t *)payload, payloadLength);

    // Read the status code exactly once.
    int statusCode = httpClient.responseStatusCode();
    //Logger.Debug("Finished HttpRequest with status code %d", statusCode);

    // Always drain the response body so no bytes are left pending in the WiFi
    // receive buffer; only log it on a non-2xx status. Pass it as a "%s" argument
    // (not as the format string) so a body containing '%' is safe. A successful
    // OTLP/HTTP export returns 200 with an empty or {"partialSuccess":{}} body.
    String responseBody = httpClient.responseBody();
    if(statusCode >= 300) {
        Logger.Error("%s", responseBody.c_str());
    }

    // Release the connection so socket buffers do not accumulate between the
    // 30s publish cycles.
    httpClient.stop();

    // Re-arm the hardware watchdog now that the network I/O is done.
    Watchdog.enable(WATCHDOG_TIMEOUT_MS);
    return statusCode;
}

void onNetworkConnect() {
    Logger.LogNetworkInformation();
    setRTC(true);
}

bool setRTC(void *argument) {
    setRTC(false);
    return true;
}

void setRTC(bool waitOnRTC) { // get the time from Internet Time Service
    unsigned long epoch;
    do {
        // Keep the dog fed while waiting on NTP so a prolonged outage doesn't
        // reboot the board mid-wait (preserves the existing retry behavior).
        Watchdog.reset();
        epoch = WiFi.getTime(); // The RTC is set to GMT or 0 Time Zone and stays at GMT.
        if(epoch == 0) {
            Logger.Warning("NTP was unavailable will try again later.");

            if(waitOnRTC) {
                delay(1000);
            }
        }
    } while (epoch == 0 && waitOnRTC);

    if(epoch != 0) {
        Logger.Info("Updating RTC from NTP server with Epoch of: %d", epoch);
        rtc.setEpoch(epoch);
    }
}