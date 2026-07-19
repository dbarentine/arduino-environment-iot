#include <sstream>
//#include <format>
#include <Arduino.h>
#include <Wire.h>
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
    : sensor(nullptr), usesMultiplexer(false), multiplierChannel(0), location(), i2cAddress(0) {
}

TempHumditySensor::TempHumditySensor(std::unique_ptr<SHT35> sensor, std::string location, uint8_t i2cAddress)
    : sensor(std::move(sensor)), usesMultiplexer(false), multiplierChannel(0), location(std::move(location)), i2cAddress(i2cAddress) {
}

TempHumditySensor::TempHumditySensor(std::unique_ptr<SHT35> sensor, ushort channel, std::string location, uint8_t i2cAddress)
    : sensor(std::move(sensor)), usesMultiplexer(true), multiplierChannel(channel), location(std::move(location)), i2cAddress(i2cAddress) {
}

DustSensor::DustSensor()
        : sensor(nullptr), usesMultiplexer(false), multiplierChannel(0), location(), i2cAddress(0) {
}

DustSensor::DustSensor(std::unique_ptr<HM330X> sensor, std::string location, uint8_t i2cAddress)
        : sensor(std::move(sensor)), usesMultiplexer(false), multiplierChannel(0), location(std::move(location)), i2cAddress(i2cAddress) {
}

DustSensor::DustSensor(std::unique_ptr<HM330X> sensor, ushort channel, std::string location, uint8_t i2cAddress)
        : sensor(std::move(sensor)), usesMultiplexer(true), multiplierChannel(channel), location(std::move(location)), i2cAddress(i2cAddress) {
}

SensorService::SensorService(SerialLogger &logger, bool multiplexerEnabled, uint8_t multiplexerAddress)
        : logger(logger), multiplexerEnabled(multiplexerEnabled), multiplexer(multiplexerAddress) {
}

// Enable the SAMD21 SERCOM master SCL-low ("SMBus host") timeout on the I2C bus
// that Wire uses (PERIPH_WIRE == sercom2 on the MKR WiFi 1010, so the register
// bank is SERCOM2). Without it, a bus held low by a wedged, mis-wired, or absent
// sensor makes the SERCOM master spin forever in its INTFLAG wait loops (see the
// Arduino core's SERCOM.cpp) — that hangs the read tick until the ~16s hardware
// watchdog reboots the board, and a persistent fault turns into a boot loop that
// never lets USB enumerate. With LOWTOUTEN set, the hardware aborts a stuck
// transfer (~25 ms) and Wire returns an error instead of blocking.
//
// CTRLA is enable-protected, so the peripheral must be disabled to change it;
// we then re-enable and force the bus back to IDLE exactly as
// SERCOM::enableWIRE() does. Call this AFTER all Wire.begin()/sensor init(), as
// each Wire.begin() resets CTRLA and would clear the bit.
static void enableI2cBusTimeout() {
    SERCOM2->I2CM.CTRLA.bit.ENABLE = 0;
    while (SERCOM2->I2CM.SYNCBUSY.bit.ENABLE) { }

    SERCOM2->I2CM.CTRLA.bit.LOWTOUTEN = 1;   // abort if SCL is held low too long

    SERCOM2->I2CM.CTRLA.bit.ENABLE = 1;
    while (SERCOM2->I2CM.SYNCBUSY.bit.ENABLE) { }

    // Force the bus state to IDLE so the next transfer can start.
    SERCOM2->I2CM.STATUS.bit.BUSSTATE = 1;
    while (SERCOM2->I2CM.SYNCBUSY.bit.SYSOP) { }
}

// Quick address ACK check: returns true only if a device ACKs at `address`.
// endTransmission() returns 0 on ACK; anything else (NACK, bus error, or a
// SERCOM SCL-low timeout) means "not present / not responding". Paired with
// enableI2cBusTimeout() this can never block, so callers can skip a dead or
// absent sensor instead of entering the driver/library retry loops (e.g.
// Seeed_SHT35's `while (Wire.endTransmission() == NACK_ON_ADDR)`) that would
// otherwise spin until the watchdog reboots.
static bool sensorResponds(uint8_t address) {
    Wire.beginTransmission(address);
    return Wire.endTransmission() == 0;
}

// Appends one OTLP NumberDataPoint (gauge, asDouble) with sensor.name + location
// attributes to a per-metric data-point buffer. `first` tracks the comma between
// data points. Values are formatted via stringstream (reliable float formatting
// on SAMD, unlike snprintf("%f") which needs float printf support linked in).
static void appendDataPoint(std::stringstream& dps, bool& first, double value, unsigned long epoch,
                            const char* name, const char* location) {
    if (!first) {
        dps << ",";
    }
    first = false;
    // timeUnixNano = epoch seconds * 1e9, built as a string (OTLP/JSON requires
    // 64-bit fields as strings) by appending nine zeros — no 64-bit math needed.
    dps << "{\"asDouble\":" << value
        << ",\"timeUnixNano\":\"" << epoch << "000000000\","
        << "\"attributes\":["
        << "{\"key\":\"sensor.name\",\"value\":{\"stringValue\":\"" << name << "\"}},"
        << "{\"key\":\"location\",\"value\":{\"stringValue\":\"" << location << "\"}}"
        << "]}";
}

// Appends one gauge metric to the metrics array (skipped if it has no data
// points, e.g. no sensors of that kind are configured).
static void appendGauge(std::stringstream& ss, bool& first, const char* name, const std::string& dataPoints) {
    if (dataPoints.empty()) {
        return;
    }
    if (!first) {
        ss << ",";
    }
    first = false;
    ss << "{\"name\":\"" << name << "\",\"gauge\":{\"dataPoints\":[" << dataPoints << "]}}";
}

int SensorService::readAndPublishSensors(int(*publish)(const std::string& str), const char* serviceName) {
    // One timestamp for the whole batch, so all points share a consistent time.
    const unsigned long epoch = rtc.getEpoch();

    // Accumulate data points per metric across all sensors, then emit one gauge
    // metric per measurement (grouping keeps the OTLP payload compact).
    std::stringstream temperatureDps, humidityDps, pm1_0Dps, pm2_5Dps, pm10Dps;
    bool temperatureFirst = true, humidityFirst = true, pm1_0First = true, pm2_5First = true, pm10First = true;

    for (const auto &tempSensor: temperatureHumiditySensors) {
        auto[temperature, humidity] = readTemperatureHumiditySensor(tempSensor.first);
        logger.Debug("%s - Temperature: %.2f, Humidity: %.2f%%", tempSensor.first.c_str(), temperature, humidity);

        const char* name = tempSensor.first.c_str();
        const char* location = tempSensor.second.location.c_str();
        appendDataPoint(temperatureDps, temperatureFirst, temperature, epoch, name, location);
        appendDataPoint(humidityDps, humidityFirst, humidity, epoch, name, location);
    }

    for (const auto &dustSensor: dustSensors) {
        auto[pm1_0_spm, pm2_5_spm, pm10_spm, pm1_0_ae, pm2_5_ae, pm10_ae] = readDustSensor(dustSensor.first);
        logger.Debug("%s - PM1.0 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm1_0_ae);
        logger.Debug("%s - PM2.5 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm2_5_ae);
        logger.Debug("%s - PM10 concentration(Atmospheric environment,unit:ug/m3): %d", dustSensor.first.c_str(),
                     pm10_ae);

        const char* name = dustSensor.first.c_str();
        const char* location = dustSensor.second.location.c_str();
        appendDataPoint(pm1_0Dps, pm1_0First, pm1_0_ae, epoch, name, location);
        appendDataPoint(pm2_5Dps, pm2_5First, pm2_5_ae, epoch, name, location);
        appendDataPoint(pm10Dps, pm10First, pm10_ae, epoch, name, location);
    }

    // Assemble the OTLP/HTTP JSON ExportMetricsServiceRequest. service.name maps
    // to the Prometheus `job` label; metric-name dots become underscores.
    std::stringstream ss;
    ss << "{\"resourceMetrics\":[{"
       << "\"resource\":{\"attributes\":[{\"key\":\"service.name\",\"value\":{\"stringValue\":\""
       << serviceName << "\"}}]},"
       << "\"scopeMetrics\":[{\"scope\":{\"name\":\"" << serviceName << "\"},\"metrics\":[";

    bool metricFirst = true;
    appendGauge(ss, metricFirst, "environment.temperature_fahrenheit", temperatureDps.str());
    appendGauge(ss, metricFirst, "environment.humidity_percent", humidityDps.str());
    appendGauge(ss, metricFirst, "environment.pm1_0_ugm3", pm1_0Dps.str());
    appendGauge(ss, metricFirst, "environment.pm2_5_ugm3", pm2_5Dps.str());
    appendGauge(ss, metricFirst, "environment.pm10_ugm3", pm10Dps.str());

    ss << "]}]}]}";

    // Bind the payload to a const local so it is passed to the callback by
    // reference (see publish signature) rather than copied by value.
    const std::string payload = ss.str();
    return publish(payload);
}

std::tuple<float, float> SensorService::readTemperatureHumiditySensor(const std::string& name) {
    float temperature = 0, humidity = 0;

    // Look the sensor up once instead of re-running operator[] (which also
    // avoids default-inserting a phantom entry on a miss).
    auto it = temperatureHumiditySensors.find(name);
    if (it == temperatureHumiditySensors.end()) {
        return {0.f, 0.f};
    }
    auto& s = it->second;

    if(multiplexerEnabled && s.usesMultiplexer) {
        //logger.Debug("Selecting multiplexer channel: %d", s.multiplierChannel);
        multiplexer.selectChannel(s.multiplierChannel);
    }

    if (s.sensor != nullptr) {
        // Probe the address first so an absent/unresponsive sensor is skipped
        // instead of blocking in the SHT35 library's NACK-retry loop. On skip or
        // read failure, temperature stays 0 (→ 32F below), matching the prior
        // read-failure behavior.
        if (!sensorResponds(s.i2cAddress)) {
            logger.Warning("Temp/humidity sensor %s (0x%02X) not responding; skipping read",
                           name.c_str(), s.i2cAddress);
        } else if (NO_ERROR !=
                   s.sensor->read_meas_data_single_shot(HIGH_REP_WITH_STRCH, &temperature, &humidity)) {
            logger.Warning("Read failed for sensor %s", name.c_str());
        }
    }

    temperature = (temperature * 1.8) + 32;

    if(multiplexerEnabled && s.usesMultiplexer) {
        //logger.Debug("Disabling multiplexer channel: %d", s.multiplierChannel);
        multiplexer.disableChannel(s.multiplierChannel);
    }

    return {temperature, humidity};
}

std::tuple<uint16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint16_t> SensorService::readDustSensor(const std::string& name) {
    uint16_t pm1_0_spm = 0;
    uint16_t pm2_5_spm = 0;
    uint16_t pm10_spm = 0;
    uint16_t pm1_0_ae = 0;
    uint16_t pm2_5_ae = 0;
    uint16_t pm10_ae = 0;

    // Single lookup instead of repeated operator[] calls.
    auto it = dustSensors.find(name);
    if (it == dustSensors.end()) {
        return {pm1_0_spm, pm2_5_spm, pm10_spm, pm1_0_ae, pm2_5_ae, pm10_ae};
    }
    auto& s = it->second;

    if(multiplexerEnabled && s.usesMultiplexer) {
        //logger.Debug("Selecting multiplexer channel: %d", s.multiplierChannel);
        multiplexer.selectChannel(s.multiplierChannel);
    }

    if (s.sensor != nullptr && !sensorResponds(s.i2cAddress)) {
        // Skip an absent/unresponsive dust sensor rather than blocking in the
        // library read path.
        logger.Warning("Dust sensor %s (0x%02X) not responding; skipping read",
                       name.c_str(), s.i2cAddress);
    } else if (s.sensor != nullptr &&
        NO_ERROR == s.sensor->read_sensor_value(dustSensorBuffer, 29)) {
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

    if(multiplexerEnabled && s.usesMultiplexer) {
        //logger.Debug("Disabling multiplexer channel: %d", s.multiplierChannel);
        multiplexer.disableChannel(s.multiplierChannel);
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
    // The struct owns its sensor via unique_ptr and is move-only, so emplace it.
    temperatureHumiditySensors.emplace(
        std::move(name),
        TempHumditySensor(std::unique_ptr<SHT35>(new SHT35(SCLPIN, IIC_ADDR)), std::move(location), IIC_ADDR));
    return this;
}

SensorService *SensorService::addTemperatureHumiditySensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR) {
    temperatureHumiditySensors.emplace(
        std::move(name),
        TempHumditySensor(std::unique_ptr<SHT35>(new SHT35(SCLPIN, IIC_ADDR)), channel, std::move(location), IIC_ADDR));
    return this;
}

SensorService *SensorService::addDustSensor(std::string name, std::string location, uint8_t IIC_ADDR) {
    dustSensors.emplace(
        std::move(name),
        DustSensor(std::unique_ptr<HM330X>(new HM330X(IIC_ADDR)), std::move(location), IIC_ADDR));
    return this;
}

SensorService *SensorService::addDustSensor(std::string name, std::string location, ushort channel, uint8_t IIC_ADDR) {
    dustSensors.emplace(
        std::move(name),
        DustSensor(std::unique_ptr<HM330X>(new HM330X(IIC_ADDR)), channel, std::move(location), IIC_ADDR));
    return this;
}

bool SensorService::InitializeSensors() {
    bool isSuccessful = true;

    // Ensure the I2C peripheral is up before the sensor libraries touch it (they
    // also call Wire.begin() in their init(), which is idempotent).
    Wire.begin();

    if(multiplexerEnabled && !multiplexer.begin()) {
        logger.Error("Unable to initialize multiplexer");
        isSuccessful = false;
    }

    for (auto const &tempSensor: temperatureHumiditySensors) {
        auto& ths = tempSensor.second;
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
        auto& ds = dustSensor.second;
        if(multiplexerEnabled && ds.usesMultiplexer) {
            //logger.Debug("Selecting multiplexer channel: %d", ds.multiplierChannel);
            multiplexer.selectChannel(ds.multiplierChannel);
        }

        if (ds.sensor->init()) {
            logger.Error("Unable to initialize sensor: %s", dustSensor.first.c_str());
            isSuccessful = false;
        }

        if(multiplexerEnabled && ds.usesMultiplexer) {
            //logger.Debug("Disabling multiplexer channel: %d", ds.multiplierChannel);
            multiplexer.disableChannel(ds.multiplierChannel);
        }
    }

    // Arm the SERCOM SCL-low timeout last: each Wire.begin()/sensor init() above
    // resets CTRLA, so enabling it here makes it stick for the runtime reads.
    enableI2cBusTimeout();

    return isSuccessful;
}

SensorService::~SensorService() {
    if(multiplexerEnabled) {
        for (int chan = 0; chan < 8; chan++) {
            multiplexer.disableChannel(chan);
            delay(100);
        }
    }

    // Sensors are owned by unique_ptr inside the structs, so they are released
    // automatically when the maps are destroyed; no manual delete needed.
}
