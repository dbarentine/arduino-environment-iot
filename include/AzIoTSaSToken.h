// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#ifndef AZIOTSASTOKEN_H
#define AZIOTSASTOKEN_H

#include <Arduino.h>
#include <az_iot_hub_client.h>
#include <az_span.h>
#include <RTCZero.h>
#include "SerialLogger.h"

class AzIoTSasToken {
public:
    AzIoTSasToken(char *broker, char *deviceKey, char *deviceId, SerialLogger &logger);

    void InitializeIoTHubClient();

    int Generate(unsigned int expiryTimeInMinutes);

    bool IsExpired();

    const char *GetSasToken();

    const char *GetUsername();

    const char *GetClientId();

private:
    az_span generateSasToken(unsigned int expiryTimeInMinutes);

    uint32_t getExpiration(uint32_t minutes);

    uint32_t getSasTokenExpiration(const char *sasToken);

    int generateSignedSignature(
            az_span sas_base64_encoded_key,
            az_span sas_signature,
            az_span sas_base64_encoded_signed_signature,
            az_span *out_sas_base64_encoded_signed_signature);

    void base64Encode(az_span decoded_bytes, az_span base64_encoded_bytes, az_span *out_base64_encoded_bytes);

    int decodeBase64(az_span base64_encoded_bytes, az_span decoded_bytes, az_span *out_decoded_bytes);

    void signSignature(az_span decoded_key, az_span signature, az_span signed_signature, az_span *out_signed_signature);

    void hmacSHA256(az_span key, az_span payload, az_span signed_payload);

    RTCZero rtc;
    SerialLogger &logger;
    az_iot_hub_client client;
    char *broker;
    az_span deviceKey;
    char *deviceId;
    uint8_t signatureBuffer[256];
    az_span signatureSpan;
    char client_id[128];
    char username[128];
    char sasTokenBuffer[200];
    az_span sasTokenSpan;
    az_span sasToken;
    uint32_t expirationUnixTime;
};

#endif // AZIOTSASTOKEN_H