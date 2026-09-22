/*
 * ota.h - update the firmware over the air.
 *
 * Holding the screen button reboots into UPDATE MODE: the dashboard brings up
 * its own WPA2 access point, serves a small upload page, and writes whatever
 * image is posted to the spare app slot. The phone side is any browser.
 *
 * Rebooting into update mode rather than tearing down the running radio is
 * deliberate. The BLE controller executes from flash
 * (CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY) and erasing flash stalls the cache, so a
 * controller left running during an update can starve. Coming up fresh with BLE
 * simply never started removes that whole class of problem, needs no teardown
 * code, and leaves the most heap free for the Wi-Fi stack.
 *
 * The CAN driver IS started in update mode, listen-only as always, so the
 * handlebar button still works to cancel and the standstill check stays live.
 * Its ISR is also flash-resident, so some frames are lost during erases; that
 * costs nothing while parked and cannot disturb a bus we never acknowledge.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "screens.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True when this boot was asked for by ota_request_update_mode(). Call once,
 * early: it clears the request, so a power cycle always comes up normally.
 * Always false when OTA is disabled at build time.
 */
bool ota_boot_is_update_mode(void);

/*
 * Report the running slot and, if the image is on probation, that it has not
 * been confirmed yet. Safe to call whether or not OTA is enabled.
 */
void ota_log_boot_state(void);

/*
 * Bring up the access point and the upload server. Only call this when
 * ota_boot_is_update_mode() returned true.
 */
esp_err_t ota_update_mode_start(void);

/*
 * Ask for update mode: checks that the bike is stationary, then reboots. Returns
 * ESP_ERR_INVALID_STATE while moving or while the running image is still on
 * probation, and ESP_ERR_NOT_SUPPORTED when OTA is disabled. On success it does
 * not return.
 */
esp_err_t ota_request_update_mode(void);

/* Leave update mode by rebooting into normal operation. Does not return. */
void ota_leave_update_mode(void);

/* True while this boot is serving updates. */
bool ota_update_mode_active(void);

/*
 * Housekeeping, from a task that runs a few times a second: confirms a
 * probationary image once it has proved itself, and reboots out of update mode
 * when it has been idle too long.
 */
void ota_tick(void);

/* Snapshot for the display. Zeroed when update mode is not active. */
void ota_get_update_data(update_data_t *out);

#ifdef __cplusplus
}
#endif
