#include "log_writer.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RING_BYTES (CONFIG_S2_LOG_QUEUE_LEN * 160)

static RingbufHandle_t s_ring;
static volatile uint32_t s_dropped;

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t len = 0;
        char *item = xRingbufferReceive(s_ring, &len, portMAX_DELAY);
        if (!item) {
            continue;
        }
        fwrite(item, 1, len, stdout);
        vRingbufferReturnItem(s_ring, item);
        /* Write everything that is already queued, then flush once the queue is drained. */
        while ((item = xRingbufferReceive(s_ring, &len, 0)) != NULL) {
            fwrite(item, 1, len, stdout);
            vRingbufferReturnItem(s_ring, item);
        }
        fflush(stdout);
    }
}

esp_err_t log_writer_start(void)
{
    if (s_ring) {
        return ESP_ERR_INVALID_STATE;
    }
    s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_ring) {
        return ESP_ERR_NO_MEM;
    }
    setvbuf(stdout, NULL, _IOFBF, 1024);
    if (xTaskCreate(writer_task, "log_writer", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Append a newline and hand the line to the writer task (or stdout before it runs). */
static bool send_line(char *buf, size_t len)
{
    if (len > LOG_LINE_MAX - 1) {
        len = LOG_LINE_MAX - 1;
    }
    buf[len++] = '\n';
    if (!s_ring) {
        fwrite(buf, 1, len, stdout);
        return true;
    }
    if (xRingbufferSend(s_ring, buf, len, 0) != pdTRUE) {
        s_dropped++;
        return false;
    }
    return true;
}

bool log_vline(const char *fmt, va_list ap)
{
    char buf[LOG_LINE_MAX + 1];
    int n = vsnprintf(buf, LOG_LINE_MAX, fmt, ap);
    if (n < 0) {
        return false;
    }
    return send_line(buf, (size_t)n);
}

bool log_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = log_vline(fmt, ap);
    va_end(ap);
    return ok;
}

bool log_tline(const char *fmt, ...)
{
    char buf[LOG_LINE_MAX + 1];
    int64_t us = esp_timer_get_time();
    int n = snprintf(buf, LOG_LINE_MAX, "[%6lld.%03lld] ", (long long)(us / 1000000), (long long)((us / 1000) % 1000));
    if (n < 0 || n >= LOG_LINE_MAX) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, (size_t)(LOG_LINE_MAX - n), fmt, ap);
    va_end(ap);
    if (m < 0) {
        return false;
    }
    size_t len = (size_t)n + (size_t)m;
    if (len > (size_t)LOG_LINE_MAX - 1) {
        len = LOG_LINE_MAX - 1;   /* body truncated: keep the head */
    }
    return send_line(buf, len);
}

uint32_t log_writer_dropped(void)
{
    return s_dropped;
}
