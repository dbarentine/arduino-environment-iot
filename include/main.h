#include <SNU.h>
#include <Arduino.h>
#include <Arduino_ConnectionHandler.h>
#include <string>
#include "arduino_secrets.h"

// Declarations only — the definitions live in main.cpp so that including this
// header from more than one translation unit does not cause multiple-definition
// linker errors.
extern const char SSID[];               // Network SSID (name)
extern const char PASS[];               // Network password (use for WPA, or use as key for WEP)

extern const char OTEL_HOST[];          // OpenTelemetry Collector host/IP on the LAN
extern const int OTEL_PORT;             // Collector OTLP/HTTP port (e.g. 4318)
extern const char OTEL_METRICS_PATH[];  // OTLP metrics path ("/v1/metrics")
extern const char OTEL_SVC_NAME[];      // OTLP resource service.name

extern int status;                      // the Wifi radio's status

void setup();

void loop();

bool readSensors(void *argument);

void onNetworkConnect();

bool setRTC(void *argument);
void setRTC(bool waitOnRTC);

int publishMessage(const std::string& str);

/*SAMD core*/
#ifdef ARDUINO_SAMD_VARIANT_COMPLIANCE
#define SDAPIN  20
#define SCLPIN  21
#define RSTPIN  7
//#define SERIAL SerialUSB
#else
#define SDAPIN  A4
#define SCLPIN  A5
#define RSTPIN  2
//#define SERIAL Serial
#endif