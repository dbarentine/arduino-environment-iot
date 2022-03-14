#ifndef SENSORSERVICE_H
#define SENSORSERVICE_H

// We have to fix a conflict between the two Seeed Libraries
#define SEEED_PM2_5_SENSOR_HM3301_HM330X_ERROR_CODE_H

#include <map>
#include <string>
#include <RTCZero.h>
#include <Seeed_SHT35.h>
#include <TCA9548.h>

typedef err_t HM330XErrorCode;

#include <Seeed_HM330X.h>
#include "SerialLogger.h"

struct TempHumditySensor {
    TempHumditySensor();
    TempHumditySensor(SHT35 *sensor, std::string location);
    TempHumditySensor(SHT35 *sensor, ushort channel, std::string location);

    SHT35 *sensor;
    bool usesMultiplexer;
    ushort multiplierChannel;
    std::string location;
};

struct DustSensor {
    DustSensor();
    DustSensor(HM330X *sensor, std::string location);
    DustSensor(HM330X *sensor, ushort channel, std::string location);

    HM330X *sensor;
    bool usesMultiplexer;
    ushort multiplierChannel;
    std::string location;
};

class SensorService {
public:
    SensorService(SerialLogger &logger, bool multiplexerEnabled, uint8_t multiplexerAddress = 0x70);

    ~SensorService();

    SensorService *addTemperatureHumiditySensor(std::string name, std::string location, uint8_t IIC_ADDR);
    SensorService *addTemperatureHumiditySensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR);

    SensorService *addDustSensor(std::string name, std::string location, uint8_t IIC_ADDR);
    SensorService *addDustSensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR);

    std::tuple<float, float> readTemperatureHumiditySensor(std::string name);

    std::tuple<uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t> readDustSensor(std::string name);

    bool InitializeSensors();

    int readAndPublishSensors(int(*publish)(std::string str));

private:
    RTCZero rtc;
    bool multiplexerEnabled;
    TCA9548 multiplexer;
    std::map<std::string, TempHumditySensor> temperatureHumiditySensors;
    std::map<std::string, DustSensor> dustSensors;
    uint8_t dustSensorBuffer[30];
    SerialLogger &logger;
};

#endif