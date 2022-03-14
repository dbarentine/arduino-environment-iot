#ifndef SERIALLOGGER_H
#define SERIALLOGGER_H

#include <Arduino.h>
#include <RTCZero.h>
#include <LibPrintf.h>

#define LOG_LEVEL_DEBUG "[DEBUG]"
#define LOG_LEVEL_INFO "[INFO]"
#define LOG_LEVEL_WARN "[WARNING]"
#define LOG_LEVEL_ERROR "[ERROR]"

class SerialLogger {
public:
    SerialLogger();

    SerialLogger(Print &PrintClass);

    void Debug(const char *format, ...);

    void Info(const char *format, ...);

    void Warning(const char *format, ...);

    void Error(const char *format, ...);

    void LogNetworkInformation();

private:
    RTCZero rtc;
    Print &PrintClass;

    void printTime();

    void printMessage(const char *logLevel, const char *format, va_list args);

    void printMacAddress(byte mac[]);
};

#endif