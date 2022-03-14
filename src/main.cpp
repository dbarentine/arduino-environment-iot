#include "main.h"
#include <RTCZero.h>
#include <Wire.h>
#include <Seeed_SHT35.h>
#include <arduino-timer.h>
#include <ArduinoJson.h>
//#include <MemoryFree.h>

#include "AzIoTSaSToken.h"
#include "SensorService.h"
#include "SerialLogger.h"

WiFiConnectionHandler conMan(SSID, PASS);
RTCZero rtc;
SerialLogger Logger;

auto timer = timer_create_default();

WiFiSSLClient wifiSSLClient;
MqttClient mqttClient(wifiSSLClient);

SensorService sensors(Logger, true);
AzIoTSasToken sasToken(iot_broker, iot_devicekey, iot_deviceId, Logger);

#define SAS_TOKEN_DURATION_IN_MINUTES 60

void setup() {
    Serial.begin(115200);
    delay(5000);

    Logger.Info("Startup");
    sensors.addTemperatureHumiditySensor("sensor1", "crawlspace", 0, 0x45);
//           ->addTemperatureHumiditySensor("sensor2", "crawlspace", 1, 0x45)
//           ->addTemperatureHumiditySensor("sensor3", "crawlspace", 2, 0x45);

    sensors.addDustSensor("dustsensor1", "crawlspace", 0x40);
    if (!sensors.InitializeSensors()) {
        Logger.Error("Sensor initialization failed");
    }

    sasToken.InitializeIoTHubClient();
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

    if (!mqttClient.connected() || sasToken.IsExpired()) {
        // MQTT client is disconnected, connect
        connectMQTT();
    }

    if (rtc.getMinutes() == 59 && rtc.getSeconds() == 0) {
        setRTC(false); // get NTP time every hour at minute 59
    }

    mqttClient.poll();
    timer.tick();
}

bool readSensors(void *argument) {
    sensors.readAndPublishSensors(&publishMessage);
    Logger.Debug("Finished reading and publishing sensors");

    if(mqttClient.getWriteError() != 0) {
        Logger.Error("MqttClient write error %d", mqttClient.getWriteError());
    }

    return true;
}

int publishMessage(std::string str) {
    //Logger.Debug("Publishing message to IoT Hub with size %d", strlen(str.c_str()));
    //Logger.Debug(str.c_str());
    mqttClient.beginMessage(iot_topic, false);
    mqttClient.write((uint8_t *) str.c_str(), strlen(str.c_str()));
    return mqttClient.endMessage();
}

void onNetworkConnect() {
    Logger.LogNetworkInformation();

    rtc.begin();
    setRTC(true);
    connectMQTT();

    timer.every(1000 * 30, readSensors);
}

void connectMQTT() {
    do {
        mqttClient.stop();
        Logger.Info("Attempting to connect to MQTT broker: %s", iot_broker);

        if (sasToken.Generate(SAS_TOKEN_DURATION_IN_MINUTES) != 0) {
            Logger.Error("Failed generating SAS token");
            return;
        }

        mqttClient.setId(sasToken.GetClientId());
        mqttClient.setUsernamePassword(sasToken.GetUsername(), sasToken.GetSasToken());

        if (!mqttClient.connect(iot_broker, 8883)) {
            Logger.Error("MQTT connection error: %d", mqttClient.connectError());
            delay(1000);
        }
    } while (!mqttClient.connected());

    Logger.Info("Successfully connected to MQTT broker: %s", iot_broker);
    //Logger.Info("Subscribing to topic: %s", iot_topic.c_str());
    // TODO: Add receive message capabilities
    //mqttClient.subscribe(iot_topic.c_str());
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