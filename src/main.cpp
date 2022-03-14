#include "main.h"
#include <RTCZero.h>
#include <Wire.h>
#include <arduino-timer.h>
#include <ArduinoHttpClient.h>

#include "SensorService.h"
#include "SerialLogger.h"

WiFiConnectionHandler conMan(SSID, PASS);
RTCZero rtc;
SerialLogger Logger;

auto timer = timer_create_default();

#ifdef MOCK_INFLUXDB_API
WiFiClient wiFiClient;
IPAddress ipAddress(10, 10, 5, 164);
HttpClient httpClient(wiFiClient, ipAddress, 3001);
#else
WiFiSSLClient wifiSSLClient;
HttpClient httpClient(wifiSSLClient, INFLUXDB_HOST, 443);
#endif

SensorService sensors(Logger, true);

void setup() {
    Serial.begin(115200);
    delay(5000);

    Logger.Info("Startup");
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
}

void loop() {
    conMan.check();

    if (WiFi.status() != WL_CONNECTED) {
        Logger.Info("Waiting on WiFi connection");
        delay(1000);
        return;
    }

    if (rtc.getMinutes() == 59 && rtc.getSeconds() == 0) {
        setRTC(false); // get NTP time every hour at minute 59
    }

    timer.tick();
}

bool readSensors(void *argument) {
    int statusCode = sensors.readAndPublishSensors(&publishMessage);
    Logger.Debug("Finished reading and publishing sensors with a status code of %d", statusCode);
    return true;
}

int publishMessage(std::string str) {
    const char* payload = str.c_str();
    size_t payloadLength = strlen(payload);

    Logger.Debug("Starting HttpRequest to %s", INFLUXDB_URL.c_str());
    //Logger.Debug("Payload %s", payload);
    httpClient.beginRequest();
    httpClient.post(INFLUXDB_URL.c_str());
    httpClient.sendHeader("Content-Type", "text/plain");
    httpClient.sendHeader("Authorization", INFLUXDB_TOKEN);
    httpClient.sendHeader(HTTP_HEADER_CONTENT_LENGTH, payloadLength);
    httpClient.beginBody();
    httpClient.write((const uint8_t *)payload, payloadLength);

    int statusCode = httpClient.responseStatusCode();
    //Logger.Debug("Finished HttpRequest with status code %d", statusCode);

    if(httpClient.responseStatusCode() >= 300) {
        Logger.Error(httpClient.responseBody().c_str());
    }

    return statusCode;
}

void onNetworkConnect() {
    Logger.LogNetworkInformation();

    rtc.begin();
    setRTC(true);

    timer.every(1000 * 30, readSensors);
}

void setRTC(bool failIfUnavailable) { // get the time from Internet Time Service
    unsigned long epoch;
    int numberOfTries = 0, maxTries = 10;
    do {
        epoch = WiFi.getTime(); // The RTC is set to GMT or 0 Time Zone and stays at GMT.
        numberOfTries++;
        delay(1000);
    } while ((epoch == 0) && (numberOfTries < maxTries));

    if (numberOfTries == maxTries) {
        if (failIfUnavailable) {
            Logger.Error("NTP unreachable!!");
            while (1);  // hang
        } else {
            Logger.Warning("NTP was unavailable will try again later.");
        }
    } else {
        Logger.Info("Updating RTC from NTP server with Epoch of: %d", epoch);
        rtc.setEpoch(epoch);
    }
}