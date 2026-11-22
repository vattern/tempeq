#pragma once
#include <stdbool.h>
#include <stdint.h>

// Bring up BLE and start advertising.
void ble_start(void);

// Turn advertising on/off (a connected client is not affected).
void ble_set_advertising(bool on);

// Time since the current client connected or last made a request, or -1 if
// no client is connected.
int64_t ble_idle_us(void);

// Disconnect any client and shut BLE down.
void ble_stop(void);
