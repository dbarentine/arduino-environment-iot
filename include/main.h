#include <SNU.h>
#include <Arduino.h>
#include <Arduino_ConnectionHandler.h>
#include <string>
#include "arduino_secrets.h"

const char SSID[] = SECRET_SSID;    // Network SSID (name)
const char PASS[] = SECRET_PASS;    // Network password (use for WPA, or use as key for WEP)

const char INFLUXDB_HOST[] = INFLUX_HOST;
const char INFLUXDB_TOKEN[] = INFLUX_TOKEN;
std::string INFLUXDB_URL = std::string("/api/v2/write?org=") + INFLUX_ORG_ID + "&bucket=" + INFLUX_BUCKET + "&precision=s";

int status = WL_IDLE_STATUS;                     // the Wifi radio's status

void setup();

void loop();

bool readSensors(void *argument);

void onNetworkConnect();

bool setRTC(void *argument);
void setRTC(bool waitOnRTC);

int publishMessage(std::string str);

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