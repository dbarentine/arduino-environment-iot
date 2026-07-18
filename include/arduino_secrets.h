// Configuration
#include <map>

#define SECRET_SSID "WifiSSID"
#define SECRET_PASS "password"

// OpenTelemetry Collector (LAN) — device POSTs OTLP/HTTP JSON here.
// OTEL_COLLECTOR_HOST must match the deployed otel-collector Service LoadBalancer IP.
#define OTEL_COLLECTOR_HOST  "10.10.4.234"
#define OTEL_COLLECTOR_PORT  4318
#define OTEL_SERVICE_NAME    "arduino-environment-iot"

std::map<const char *, std::tuple<const char*, bool, ushort, uint8_t>> tempHumiditySensors = {
        {"sensor1", {"crawlspace", true, 0, 0x45}},
        {"sensor2", {"crawlspace", true, 1, 0x45}},
        {"sensor3", {"crawlspace", true, 2, 0x45}}
};

std::map<const char *, std::tuple<const char*, bool, ushort, uint8_t>> dustSensors = {
        {"dustsensor1", {"garage", false, 0, 0x40}}
};
