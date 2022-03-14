// Configuration
#include <map>

#define SECRET_SSID "WifiSSID"
#define SECRET_PASS "password"

#define INFLUX_HOST "us-west-2-2.aws.cloud2.influxdata.com" // Influx host (e.g. eu-central-1-1.aws.cloud2.influxdata.com)
#define INFLUX_ORG_ID "orgid" // Org id
#define INFLUX_TOKEN "Token influxdb_token" // Influx token
#define INFLUX_BUCKET "environment_data" // Influx bucket that we set up for this host

std::map<const char *, std::tuple<const char*, bool, ushort, uint8_t>> tempHumiditySensors = {
        {"sensor1", {"crawlspace", true, 0, 0x45}},
        {"sensor2", {"crawlspace", true, 1, 0x45}},
        {"sensor3", {"crawlspace", true, 2, 0x45}}
};

std::map<const char *, std::tuple<const char*, bool, ushort, uint8_t>> dustSensors = {
        {"dustsensor1", {"garage", false, 0, 0x40}}
};