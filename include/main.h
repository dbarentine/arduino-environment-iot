#include <SNU.h>
#include <Arduino.h>
#include <Arduino_ConnectionHandler.h>
#include <ArduinoMqttClient.h>
#include <string>
#include "arduino_secrets.h"

const char SSID[] = SECRET_SSID;    // Network SSID (name)
const char PASS[] = SECRET_PASS;    // Network password (use for WPA, or use as key for WEP)

char iot_broker[] = SECRET_IOT_HUB_BROKER;
char iot_devicekey[] = SECRET_IOT_HUB_DEVICEKEY;
char iot_deviceId[] = SECRET_IOT_HUB_DEVICEID;
String iot_topic = "devices/" + String(iot_deviceId) + "/messages/events/";

int status = WL_IDLE_STATUS;                     // the Wifi radio's status

void setup();

void loop();

bool readSensors(void *argument);

void onNetworkConnect();

void setRTC(bool failIfUnavailable);

void connectMQTT();

int publishMessage(std::string str);
//void publishMessage(float temperature, float humidity);

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