#include <Arduino.h>
#include <WiFiNINA.h>
#include "SerialLogger.h"

SerialLogger::SerialLogger()
        : PrintClass(Serial) {
}

SerialLogger::SerialLogger(Print &PrintClass)
        : PrintClass(PrintClass) {
    printf_init(PrintClass);
}

void SerialLogger::Debug(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printMessage(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void SerialLogger::Info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printMessage(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void SerialLogger::Warning(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printMessage(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

void SerialLogger::Error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printMessage(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}

void SerialLogger::LogNetworkInformation() {
    IPAddress ip = WiFi.localIP();
    PrintClass.print("IP Address: ");
    PrintClass.println(ip);

    byte mac[6];
    WiFi.macAddress(mac);
    PrintClass.print("MAC address: ");
    printMacAddress(mac);
}

void SerialLogger::printMessage(const char *logLevel, const char *format, va_list args) {
    printTime();

    // Format into a fixed stack buffer and emit via PrintClass instead of the
    // heap-backed printf/vprintf path. snprintf/vsnprintf here resolve to
    // LibPrintf's float-capable implementation (see SerialLogger.h), so "%f"
    // formats correctly. Overlong messages are safely truncated.
    char buf[200];
    snprintf(buf, sizeof(buf), " %9s ", logLevel);
    PrintClass.print(buf);

    vsnprintf(buf, sizeof(buf), format, args);
    PrintClass.print(buf);
    PrintClass.print("\n");
}

void SerialLogger::printTime() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%02d/%02d %02d:%02d:%02d ", rtc.getMonth(), rtc.getDay(), rtc.getYear(),
             rtc.getHours(), rtc.getMinutes(), rtc.getSeconds());
    PrintClass.print(buf);
}

void SerialLogger::printMacAddress(byte mac[]) {
    for (int i = 5; i >= 0; i--) {
        if (mac[i] < 16) {
            PrintClass.print("0");
        }

        PrintClass.print(mac[i], HEX);
        if (i > 0) {
            PrintClass.print(":");
        }
    }
    PrintClass.println();
}