/*
 * can_bus.h - ESP32-S3 TWAI (CAN 2.0) node wrapper built on esp_driver_twai.
 *
 * Frames received in the driver ISR are copied into a FreeRTOS queue that the
 * decoder task drains with can_bus_receive(). Transmission is only possible when
 * the node is not in listen-only mode (CONFIG_S2_CAN_LISTEN_ONLY).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "hal/twai_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int64_t timestamp_us;   /* esp_timer time when the ISR picked the frame up */
    uint32_t id;
    uint8_t dlc;            /* 0..8 */
    bool extended;
    bool rtr;
    uint8_t data[8];
} can_frame_t;

typedef struct {
    uint32_t rx_frames;         /* frames delivered to the queue */
    uint32_t rx_queue_full;     /* frames dropped because the queue was full */
    uint32_t rx_isr_errors;     /* twai_node_receive_from_isr failures */
    uint32_t tx_frames;
    uint32_t tx_failed;
    uint32_t bus_errors;        /* on_error callbacks */
    uint32_t hal_bus_errors;    /* driver's own bus error record since enable */
    uint32_t err_arb_lost;
    uint32_t err_bit;
    uint32_t err_form;
    uint32_t err_stuff;
    uint32_t err_ack;
    uint32_t state_changes;
    uint32_t bus_off_events;
    uint32_t recoveries;
    twai_error_state_t state;   /* last reported error state */
    uint16_t tx_error_count;    /* TEC, sampled at can_bus_get_stats() (not populated on IDF 5.5.1) */
    uint16_t rx_error_count;    /* REC (idem) */
} can_bus_stats_t;

/* Create and enable the TWAI node with the Kconfig settings. */
esp_err_t can_bus_start(void);

/* Blocking receive with timeout. Returns false on timeout. */
bool can_bus_receive(can_frame_t *out, TickType_t timeout);

/*
 * Transmit a standard-id data frame. Returns ESP_ERR_NOT_SUPPORTED in
 * listen-only mode, ESP_ERR_TIMEOUT when the TX queue stayed full.
 */
esp_err_t can_bus_transmit(uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms);

/* Snapshot of counters; also refreshes TEC/REC from the driver. */
void can_bus_get_stats(can_bus_stats_t *out);

/* Call periodically from a task: starts bus-off recovery when needed. */
void can_bus_service(void);

bool can_bus_is_listen_only(void);
const char *can_bus_state_name(twai_error_state_t state);

#ifdef __cplusplus
}
#endif
