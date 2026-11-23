#pragma once
#include <stdbool.h>
#include <stddef.h>
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

// Name the logger advertises as (saved in NVS; BLE_DEVICE_NAME until set).
const char *ble_name(void);

// Set and save the advertised name: 1..BLE_NAME_MAX_LEN printable ASCII
// characters, no leading/trailing space. len 0 restores the default. Takes
// effect straight away if advertising, otherwise at the next advertising window.
bool ble_set_name(const char *name, size_t len);
