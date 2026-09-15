/*
 * ble_telemetry.h - publish the decoded vehicle state over Bluetooth LE.
 *
 * Brings up a NimBLE peripheral advertising two services: the Nordic UART
 * Service, which published Android terminal apps read without any development,
 * and a custom telemetry service for a purpose-built client. The wire formats
 * are in ble_proto.h and the contract is documented in README.md.
 *
 * The ESP32-S3 has no Bluetooth Classic radio, so there is no SPP alternative.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise NVS, the controller, the host and the GATT table, then start
 * advertising and the publisher task. Returns ESP_ERR_NOT_SUPPORTED when the
 * build has BLE switched off.
 */
esp_err_t ble_telemetry_start(void);

/* True while a client is connected. */
bool ble_telemetry_connected(void);

#ifdef __cplusplus
}
#endif
