#pragma once
#include <stdbool.h>

// Line-based command interface on the USB-Serial-JTAG port (the console).
//
//   status        -> STATUS clock_set=<0|1> now=<unix> interval=<s> count=<n> capacity=<n> next_due=<unix>
//   dump          -> ts,temp_c,humidity_pct  header, then one CSV line per reading (oldest first)
//   time <unix>   -> set the clock (UTC)
//   help
//
// Every command ends with a line "OK" or "ERR <reason>". Other lines (e.g. log
// output in the debug build) may be interleaved and should be ignored.

// Install the driver and route the console through it. Safe to call repeatedly.
void usb_cmd_start(void);

// True while a USB host is attached (receiving SOF packets). A charger or
// power bank does not count.
bool usb_cmd_host_connected(void);

// Process any pending input. Non-blocking.
void usb_cmd_poll(void);
