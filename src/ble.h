#pragma once
#include <stdbool.h>
#include <stdint.h>

// Bring up BLE and start advertising.
void ble_start(void);

// Turn advertising on/off (a connected client is not affected).
void ble_set_advertising(bool on);

// How long the current client has been connected, or -1 if none.
int64_t ble_connected_for_us(void);

// Disconnect any client and shut BLE down.
void ble_stop(void);
