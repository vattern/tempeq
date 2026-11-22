#include "usb_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "datalog.h"

static char line[64];
static size_t line_len;

void usb_cmd_start(void) {
  if (usb_serial_jtag_is_driver_installed()) return;
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
  // Send console output (printf + logs) through the driver as well, so it
  // doesn't interleave byte-by-byte with command replies.
  usb_serial_jtag_vfs_use_driver();
}

bool usb_cmd_host_connected(void) { return usb_serial_jtag_is_connected(); }

// centi-units -> "-1.23"; empty string for the error marker
static const char *fmt_centi(char *buf, size_t n, int32_t v, bool error) {
  if (error) {
    buf[0] = '\0';
  } else {
    int32_t a = v < 0 ? -v : v;
    snprintf(buf, n, "%s%ld.%02ld", v < 0 ? "-" : "", (long)(a / 100), (long)(a % 100));
  }
  return buf;
}

static void cmd_status(void) {
  datalog_flush();
  uint32_t first, next;
  datalog_range(&first, &next);
  reading_t last;
  bool have = datalog_latest(&last);
  printf("STATUS clock_set=%d now=%lu interval=%d count=%lu first=%lu next=%lu capacity=%lu next_due=%lu\n",
         datalog_time_valid() ? 1 : 0, (unsigned long)datalog_now(), TH_LOG_INTERVAL_S,
         (unsigned long)(next - first), (unsigned long)first, (unsigned long)next,
         (unsigned long)datalog_capacity(), (unsigned long)datalog_next_due(have ? &last : NULL));
  printf("OK\n");
}

// dump [from]: readings with record number >= from (default: all)
static void cmd_dump(uint32_t from) {
  datalog_flush();
  static reading_t page[64];
  uint32_t first, next, start;
  datalog_range(&first, &next);
  char t[12], h[12];
  printf("ts,temp_c,humidity_pct\n");
  uint32_t pos = from;
  for (;;) {
    uint16_t n = datalog_read(pos, page, 64, &start);
    if (n == 0) break;
    for (uint16_t i = 0; i < n; i++) {
      const reading_t *r = &page[i];
      printf("%lu,%s,%s\n", (unsigned long)r->ts,
             fmt_centi(t, sizeof(t), r->temp, r->temp == TH_TEMP_ERROR),
             fmt_centi(h, sizeof(h), r->hum, r->hum == TH_HUM_ERROR));
    }
    pos = start + n;
    if (pos >= next) break;
    vTaskDelay(1);  // long dumps: let the idle task run (task watchdog)
  }
  printf("OK\n");
}

static void handle(char *cmd) {
  while (*cmd == ' ') cmd++;
  if (strcmp(cmd, "status") == 0) {
    cmd_status();
  } else if (strcmp(cmd, "dump") == 0) {
    cmd_dump(0);
  } else if (strncmp(cmd, "dump ", 5) == 0) {
    cmd_dump((uint32_t)strtoul(cmd + 5, NULL, 10));
  } else if (strcmp(cmd, "clear") == 0) {
    datalog_clear();
    printf("OK\n");
  } else if (strncmp(cmd, "time ", 5) == 0) {
    char *end;
    unsigned long t = strtoul(cmd + 5, &end, 10);
    if (end == cmd + 5 || *end != '\0' || t < MIN_VALID_EPOCH) {
      printf("ERR expected: time <unix seconds>\n");
    } else {
      datalog_set_time((uint32_t)t);
      printf("OK\n");
    }
  } else if (strcmp(cmd, "help") == 0) {
    printf("commands: status | dump [from] | time <unix> | clear | help\nOK\n");
  } else {
    printf("ERR unknown command '%s' (try help)\n", cmd);
  }
  fflush(stdout);
}

void usb_cmd_poll(void) {
  uint8_t buf[64];
  int n;
  while ((n = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0)) > 0) {
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\r' || c == '\n') {
        if (line_len) {
          line[line_len] = '\0';
          line_len = 0;
          handle(line);
        }
      } else if (line_len < sizeof(line) - 1) {
        line[line_len++] = c;
      }
    }
  }
}
