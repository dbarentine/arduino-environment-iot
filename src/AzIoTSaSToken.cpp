// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include "AzIoTSaSToken.h"
#include <az_core.h>
#include <az_iot.h>
#include <azure_ca.h>
#include <az_result.h>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>

#include "SerialLogger.h"

#define INDEFINITE_TIME ((time_t)-1)
#define AZURE_SDK_CLIENT_USER_AGENT "c/" AZ_SDK_VERSION_STRING "(ard;mkrwifi1010)"

#define sizeofarray(a) (sizeof(a) / sizeof(a[0]))
#define is_az_span_empty(x) (az_span_size(x) == az_span_size(AZ_SPAN_EMPTY) && az_span_ptr(x) == az_span_ptr(AZ_SPAN_EMPTY))

AzIoTSasToken::AzIoTSasToken(char *broker, char *deviceKey, char *deviceId, SerialLogger &logger)
        : logger(logger) {
    this->broker = broker;
    this->deviceKey = az_span_create_from_str(deviceKey);
    this->deviceId = deviceId;
    this->signatureSpan = AZ_SPAN_FROM_BUFFER(signatureBuffer);
    this->sasTokenSpan = AZ_SPAN_FROM_BUFFER(sasTokenBuffer);
    this->expirationUnixTime = 0;
    this->sasToken = AZ_SPAN_EMPTY;
}

int AzIoTSasToken::Generate(unsigned int expiryTimeInMinutes) {
    this->sasToken = generateSasToken(expiryTimeInMinutes);
    if (is_az_span_empty(this->sasToken)) {
        logger.Error("Failed generating SAS token");
        return 1;
    } else {
        this->expirationUnixTime = getSasTokenExpiration((const char *) az_span_ptr(this->sasToken));

        if (this->expirationUnixTime == 0) {
            logger.Error("Failed getting the SAS token expiration time");
            this->sasToken = AZ_SPAN_EMPTY;
            return 1;
        } else {
            return 0;
        }
    }
}

bool AzIoTSasToken::IsExpired() {
    uint32_t now = rtc.getEpoch();
    if (now == INDEFINITE_TIME) {
        logger.Error("Failed getting current time");
        return true;
    } else {
        return (now >= this->expirationUnixTime);
    }
}

const char *AzIoTSasToken::GetSasToken() { return (const char *) az_span_ptr(sasToken); }

const char *AzIoTSasToken::GetClientId() { return (const char *) client_id; }

const char *AzIoTSasToken::GetUsername() { return (const char *) username; }

void AzIoTSasToken::InitializeIoTHubClient() {
    logger.Info("Broker: %s", broker);
    logger.Info("Device ID: %s", deviceId);
    az_iot_hub_client_options options = az_iot_hub_client_options_default();
    options.user_agent = AZ_SPAN_FROM_STR(AZURE_SDK_CLIENT_USER_AGENT);

    if (az_result_failed(az_iot_hub_client_init(
            &client,
            az_span_create((uint8_t *) broker, strlen(broker)),
            az_span_create((uint8_t *) deviceId, strlen(deviceId)),
            &options))) {
        logger.Error("Failed initializing Azure IoT Hub client");
        return;
    }

    size_t client_id_length;
    if (az_result_failed(
            az_iot_hub_client_get_client_id(&client, client_id, sizeof(client_id) - 1, &client_id_length))) {
        logger.Error("Failed getting client id");
        return;
    }

    if (az_result_failed(az_iot_hub_client_get_user_name(&client, username, sizeofarray(username), NULL))) {
        logger.Error("Failed to get MQTT clientId, return code");
        return;
    }

    logger.Info("IoT Client ID: %s", client_id);
    logger.Info("IoT Username: %s", username);
}

az_span AzIoTSasToken::generateSasToken(unsigned int expiryTimeInMinutes) {
    az_result rc;
    // Create the POSIX expiration time from input minutes.
    uint32_t sas_duration = getExpiration(expiryTimeInMinutes);

    // Get the signature that will later be signed with the decoded key.
    // az_span sas_signature = AZ_SPAN_FROM_BUFFER(signature);
    rc = az_iot_hub_client_sas_get_signature(&client, sas_duration, signatureSpan, &signatureSpan);
    if (az_result_failed(rc)) {
        logger.Error("Could not get the signature for SAS key: az_result return code " + rc);
        return AZ_SPAN_EMPTY;
    }

    // Generate the encoded, signed signature (b64 encoded, HMAC-SHA256 signing).
    char b64enc_hmacsha256_signature[64];
    az_span sas_base64_encoded_signed_signature = AZ_SPAN_FROM_BUFFER(b64enc_hmacsha256_signature);

    if (generateSignedSignature(
            deviceKey,
            signatureSpan,
            sas_base64_encoded_signed_signature,
            &sas_base64_encoded_signed_signature) != 0) {
        logger.Error("Failed generating SAS token signed signature");
        return AZ_SPAN_EMPTY;
    }

    // Get the resulting MQTT password, passing the base64 encoded, HMAC signed bytes.
    size_t mqtt_password_length;
    rc = az_iot_hub_client_sas_get_password(
            &client,
            sas_duration,
            sas_base64_encoded_signed_signature,
            AZ_SPAN_EMPTY,
            (char *) az_span_ptr(sasTokenSpan),
            az_span_size(sasTokenSpan),
            &mqtt_password_length);

    if (az_result_failed(rc)) {
        logger.Error("Could not get the password: az_result return code " + rc);
        return AZ_SPAN_EMPTY;
    } else {
        return az_span_slice(sasTokenSpan, 0, mqtt_password_length);
    }
}

uint32_t AzIoTSasToken::getExpiration(uint32_t minutes) {
    return (uint32_t) (rtc.getEpoch() + minutes * 60);
}

uint32_t AzIoTSasToken::getSasTokenExpiration(const char *sasToken) {
    const char SE[] = {'&', 's', 'e', '='};
    uint32_t se_as_unix_time = 0;

    int i, j;
    for (i = 0, j = 0; sasToken[i] != '\0'; i++) {
        if (sasToken[i] == SE[j]) {
            j++;
            if (j == sizeof(SE)) {
                // i is still at the '=' position. We must advance it by 1.
                i++;
                break;
            }
        } else {
            j = 0;
        }
    }

    if (j != sizeof(SE)) {
        logger.Error("Failed finding `se` field in SAS token");
    } else {
        int k = i;
        while (sasToken[k] != '\0' && sasToken[k] != '&') { k++; }

        if (az_result_failed(
                az_span_atou32(az_span_create((uint8_t *) sasToken + i, k - i), &se_as_unix_time))) {
            logger.Error("Failed parsing SAS token expiration timestamp");
        }
    }

    return se_as_unix_time;
}

int AzIoTSasToken::generateSignedSignature(
        az_span sas_base64_encoded_key,
        az_span sas_signature,
        az_span sas_base64_encoded_signed_signature,
        az_span *out_sas_base64_encoded_signed_signature) {
    // Decode the sas base64 encoded key to use for HMAC signing.
    char sas_decoded_key_buffer[32];
    az_span sas_decoded_key = AZ_SPAN_FROM_BUFFER(sas_decoded_key_buffer);

    if (decodeBase64(sas_base64_encoded_key, sas_decoded_key, &sas_decoded_key) != 0) {
        logger.Error("Failed generating encoded signed signature");
        return 1;
    }

    // HMAC-SHA256 sign the signature with the decoded key.
    char sas_hmac256_signed_signature_buffer[32];
    az_span sas_hmac256_signed_signature = AZ_SPAN_FROM_BUFFER(sas_hmac256_signed_signature_buffer);
    signSignature(sas_decoded_key, sas_signature, sas_hmac256_signed_signature, &sas_hmac256_signed_signature);

    // Base64 encode the result of the HMAC signing.
    base64Encode(sas_hmac256_signed_signature, sas_base64_encoded_signed_signature,
                 out_sas_base64_encoded_signed_signature);

    return 0;
}

int AzIoTSasToken::decodeBase64(az_span base64_encoded_bytes, az_span decoded_bytes, az_span *out_decoded_bytes) {
    memset(az_span_ptr(decoded_bytes), 0, (size_t) az_span_size(decoded_bytes));

    size_t len;
    if (mbedtls_base64_decode(
            az_span_ptr(decoded_bytes),
            (size_t) az_span_size(decoded_bytes),
            &len,
            az_span_ptr(base64_encoded_bytes),
            (size_t) az_span_size(base64_encoded_bytes))
        != 0) {
        logger.Error("mbedtls_base64_decode fail");
        return 1;
    } else {
        *out_decoded_bytes = az_span_create(az_span_ptr(decoded_bytes), (int32_t) len);
        return 0;
    }
}

void AzIoTSasToken::signSignature(az_span decoded_key, az_span signature, az_span signed_signature,
                                  az_span *out_signed_signature) {
    hmacSHA256(decoded_key, signature, signed_signature);
    *out_signed_signature = az_span_slice(signed_signature, 0, 32);
}

void
AzIoTSasToken::base64Encode(az_span decoded_bytes, az_span base64_encoded_bytes, az_span *out_base64_encoded_bytes) {
    size_t len;
    if (mbedtls_base64_encode(
            az_span_ptr(base64_encoded_bytes),
            (size_t) az_span_size(base64_encoded_bytes),
            &len,
            az_span_ptr(decoded_bytes),
            (size_t) az_span_size(decoded_bytes))
        != 0) {
        logger.Error("mbedtls_base64_encode fail");
    }

    *out_base64_encoded_bytes = az_span_create(az_span_ptr(base64_encoded_bytes), (int32_t) len);
}

void AzIoTSasToken::hmacSHA256(az_span key, az_span payload, az_span signed_payload) {
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *) az_span_ptr(key), az_span_size(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *) az_span_ptr(payload), az_span_size(payload));
    mbedtls_md_hmac_finish(&ctx, (byte *) az_span_ptr(signed_payload));
    mbedtls_md_free(&ctx);
}