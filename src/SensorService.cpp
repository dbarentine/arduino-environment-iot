#include <sstream>
//#include <format>
#include "SensorService.h"

#ifdef ARDUINO_SAMD_VARIANT_COMPLIANCE
#define SDAPIN  20
#define SCLPIN  21
#define RSTPIN  7
#else
#define SDAPIN  A4
#define SCLPIN  A5
#define RSTPIN  2
#endif

// const char* str[] = {"sensor num: %d",
//                      "PM1.0 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d",
//                      "PM2.5 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d",
//                      "PM10 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d",
//                      "PM1.0 concentration(Atmospheric environment,unit:ug/m3): %d",
//                      "PM2.5 concentration(Atmospheric environment,unit:ug/m3): %d",
//                      "PM10 concentration(Atmospheric environment,unit:ug/m3): %d",
//                     };

TempHumditySensor::TempHumditySensor()
    : sensor(nullptr), usesMultiplexer(false), multiplierChannel(0), location(nullptr) {
}

TempHumditySensor::TempHumditySensor(SHT35 *sensor, std::string location)
    : sensor(sensor), usesMultiplexer(false), multiplierChannel(0), location(location) {
}

TempHumditySensor::TempHumditySensor(SHT35 *sensor, ushort channel, std::string location)
    : sensor(sensor), usesMultiplexer(true), multiplierChannel(channel), location(location) {
}

DustSensor::DustSensor()
        : sensor(nullptr), usesMultiplexer(false), multiplierChannel(0), location(nullptr) {
}

DustSensor::DustSensor(HM330X *sensor, std::string location)
        : sensor(sensor), usesMultiplexer(false), multiplierChannel(0), location(location) {
}

DustSensor::DustSensor(HM330X *sensor, ushort channel, std::string location)
        : sensor(sensor), usesMultiplexer(true), multiplierChannel(channel), location(location) {
}

SensorService::SensorService(SerialLogger &logger, bool multiplexerEnabled, uint8_t multiplexerAddress)
        : logger(logger), multiplexerEnabled(multiplexerEnabled), multiplexer(multiplexerAddress) {
}

int SensorService::readAndPublishSensors(int(*publish)(std::string str)) {
    std::stringstream ss;

    for (const auto &tempSensor: temperatureHumiditySensors) {
        auto[temperature, humidity] = readTemperatureHumiditySensor(tempSensor.first);
        logger.Debug("%s - Temperature: %.2f, Humidity: %.2f%%", tempSensor.first.c_str(), temperature, humidity);

        //std::format("temperature_humidity,name=%s temperature=%f,humidity=%f 1465839830100400200", tempSensor.first, temperature, humidity);
        ss << "sensors,type=temperature_humidity,name=" << tempSensor.first << ",location=" << tempSensor.second.location << " temperature=" << temperature
           << ",humidity=" << humidity << "\n";// << rtc.getEpoch() << "\n";
    }

    for (const auto &dustSensor: dustSensors) {
        auto[pm1_0_spm, pm2_5_spm, pm10_spm, pm1_0_ae, pm2_5_ae, pm10_ae] = readDustSensor(dustSensor.first);
        // Logger.Debug("dustsensor1 - PM1.0 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d", pm1_0_spm);
        // Logger.Debug("dustsensor1 - PM2.5 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d", pm2_5_spm);
        // Logger.Debug("dustsensor1 - PM10 concentration(CF=1,Standard particulate matter,unit:ug/m3): %d", pm10_spm);
        logger.Debug("%s - PM1.0 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm1_0_ae);
        logger.Debug("%s - PM2.5 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm2_5_ae);
        logger.Debug("%s - PM10 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm10_ae);
        ss << "sensors,type=dust,name=" << dustSensor.first << ",location=" << dustSensor.second.location << " pm1_0_ae=" << pm1_0_ae << ",pm2_5_ae=" << pm2_5_ae
           << ",pm10_ae=" << pm10_ae << "\n";// << rtc.getEpoch() << "\n";
    }

    return publish(ss.str());
}

std::tuple<float, float> SensorService::readTemperatureHumiditySensor(std::string name) {
    float temperature = 0, humidity = 0;

    if(multiplexerEnabled && temperatureHumiditySensors[name].usesMultiplexer) {
        //logger.Debug("Selecting multiplexer channel: %d", temperatureHumiditySensors[name].multiplierChannel);
        multiplexer.selectChannel(temperatureHumiditySensors[name].multiplierChannel);
    }

    if (temperatureHumiditySensors[name].sensor != nullptr &&
        NO_ERROR !=
        temperatureHumiditySensors[name].sensor->read_meas_data_single_shot(HIGH_REP_WITH_STRCH, &temperature, &humidity)) {
        logger.Warning("Read failed for sensor %s", name.c_str());
    }

    temperature = (temperature * 1.8) + 32;

    if(multiplexerEnabled && temperatureHumiditySensors[name].usesMultiplexer) {
        //logger.Debug("Disabling multiplexer channel: %d", temperatureHumiditySensors[name].multiplierChannel);
        multiplexer.disableChannel(temperatureHumiditySensors[name].multiplierChannel);
    }

    return {temperature, humidity};
}

std::tuple<uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t> SensorService::readDustSensor(std::string name) {
    uint16_t pm1_0_spm = 0;
    uint16_t pm2_5_spm = 0;
    uint16_t pm10_spm = 0;
    uint16_t pm1_0_ae = 0;
    uint16_t pm2_5_ae = 0;
    uint16_t pm10_ae = 0;

    if(multiplexerEnabled && dustSensors[name].usesMultiplexer) {
        //logger.Debug("Selecting multiplexer channel: %d", dustSensors[name].multiplierChannel);
        multiplexer.selectChannel(dustSensors[name].multiplierChannel);
    }

    if (dustSensors[name].sensor != nullptr &&
        NO_ERROR == dustSensors[name].sensor->read_sensor_value(dustSensorBuffer, 29)) {
        // Get checksum from sensor read
        uint8_t sum = 0;
        for (int i = 0; i < 28; i++) {
            sum += dustSensorBuffer[i];
        }

        if (sum == dustSensorBuffer[28]) {
            // Parse sensor values out
            pm1_0_spm = (uint16_t) dustSensorBuffer[2 * 2] << 8 | dustSensorBuffer[2 * 2 + 1];
            pm2_5_spm = (uint16_t) dustSensorBuffer[3 * 2] << 8 | dustSensorBuffer[3 * 2 + 1];
            pm10_spm = (uint16_t) dustSensorBuffer[4 * 2] << 8 | dustSensorBuffer[4 * 2 + 1];
            pm1_0_ae = (uint16_t) dustSensorBuffer[5 * 2] << 8 | dustSensorBuffer[5 * 2 + 1];
            pm2_5_ae = (uint16_t) dustSensorBuffer[6 * 2] << 8 | dustSensorBuffer[6 * 2 + 1];
            pm10_ae = (uint16_t) dustSensorBuffer[7 * 2] << 8 | dustSensorBuffer[7 * 2 + 1];

            // uint16_t value = 0;
            // for (int i = 2; i < 8; i++) {
            //     value = (uint16_t) dustSensorBuffer[i * 2] << 8 | dustSensorBuffer[i * 2 + 1];
            //     logger.Debug(str[i - 1], value);
            // }
        } else {
            logger.Error("Checksum for sensor %s failed", name.c_str());
        }
    } else {
        logger.Warning("Read failed for sensor %s", name.c_str());
    }

    if(multiplexerEnabled && dustSensors[name].usesMultiplexer) {
        //logger.Debug("Disabling multiplexer channel: %d", dustSensors[name].multiplierChannel);
        multiplexer.disableChannel(dustSensors[name].multiplierChannel);
    }
/*  The standard particulate matter mass concentration value refers to the mass concentration value obtained by 
    density conversion of industrial metal particles as equivalent particles, and is suitable for use in industrial 
    production workshops and the like.
    
    The concentration of particulate matter in the atmospheric environment is converted by the density of the main 
    pollutants in the air as equivalent particles, and is suitable for ordinary indoor and outdoor atmospheric environments.
    So you can see that there are two sets of data above.*/

    return {pm1_0_spm, pm2_5_spm, pm10_spm, pm1_0_ae, pm2_5_ae, pm10_ae};
}

SensorService *SensorService::addTemperatureHumiditySensor(std::string name, std::string location, uint8_t IIC_ADDR) {
    TempHumditySensor sensor(new SHT35(SCLPIN, IIC_ADDR), location.c_str());
    temperatureHumiditySensors.insert(make_pair(name, sensor));
    return this;
}

SensorService *SensorService::addTemperatureHumiditySensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR) {
    TempHumditySensor sensor(new SHT35(SCLPIN, IIC_ADDR), channel, location.c_str());
    temperatureHumiditySensors.insert(make_pair(name, sensor));
    return this;
}

SensorService *SensorService::addDustSensor(std::string name, std::string location, uint8_t IIC_ADDR) {
    DustSensor sensor(new HM330X(IIC_ADDR), location.c_str());
    dustSensors.insert(make_pair(name, sensor));
    return this;
}

SensorService *SensorService::addDustSensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR) {
    DustSensor sensor(new HM330X(IIC_ADDR), channel, location.c_str());
    dustSensors.insert(make_pair(name, sensor));
    return this;
}

bool SensorService::InitializeSensors() {
    bool isSuccessful = true;

    if(multiplexerEnabled && !multiplexer.begin()) {
        logger.Error("Unable to initialize multiplexer");
        isSuccessful = false;
    }

    for (auto const &tempSensor: temperatureHumiditySensors) {
        TempHumditySensor ths = tempSensor.second;
        if(multiplexerEnabled && ths.usesMultiplexer) {
            //logger.Debug("Selecting multiplexer channel: %d", ths.multiplierChannel);
            multiplexer.selectChannel(ths.multiplierChannel);
        }

        if (ths.sensor->init()) {
            logger.Error("Unable to initialize sensor: %s", tempSensor.first.c_str());
            isSuccessful = false;
        }

        if(multiplexerEnabled && ths.usesMultiplexer) {
            //logger.Debug("Disabling multiplexer channel: %d", ths.multiplierChannel);
            multiplexer.disableChannel(ths.multiplierChannel);
        }
    }

    for (auto const &dustSensor: dustSensors) {
        DustSensor ds = dustSensor.second;
        if(multiplexerEnabled && ds.usesMultiplexer) {
            //logger.Debug("Selecting multiplexer channel: %d", ds.multiplierChannel);
            multiplexer.selectChannel(ds.multiplierChannel);
        }

        if (dustSensor.second.sensor->init()) {
            logger.Error("Unable to initialize sensor: %s", dustSensor.first.c_str());
            isSuccessful = false;
        }

        if(multiplexerEnabled && ds.usesMultiplexer) {
            //logger.Debug("Disabling multiplexer channel: %d", ds.multiplierChannel);
            multiplexer.disableChannel(ds.multiplierChannel);
        }
    }

    return isSuccessful;
}

SensorService::~SensorService() {
    if(multiplexerEnabled) {
        for (int chan = 0; chan < 8; chan++) {
            multiplexer.disableChannel(chan);
            delay(100);
        }
    }

    for (auto &tempSensor: temperatureHumiditySensors) {
        delete tempSensor.second.sensor;
        tempSensor.second.sensor = nullptr;
    }

    for (auto &dustSensor: dustSensors) {
        delete dustSensor.second.sensor;
        dustSensor.second.sensor = nullptr;
    }
}