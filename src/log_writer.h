/*
 * log_writer.h - decoupled console output.
 *
 * Producers format a line into a ring buffer; a low-priority task writes the
 * lines to stdout (USB Serial/JTAG). If the host is not draining the port the
 * VFS drops output after ~50 ms, and if the ring buffer is full the line is
 * dropped and counted, so CAN reception never waits on the console.
 */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_LINE_MAX 480

esp_err_t log_writer_start(void);

/* Queue one line (a newline is appended). Returns false when dropped. */
bool log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool log_vline(const char *fmt, va_list ap);

/* Same, prefixed with "[ssss.mmm] " boot-relative seconds. */
bool log_tline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint32_t log_writer_dropped(void);

#ifdef __cplusplus
}
#endif
