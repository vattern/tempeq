#pragma once
#include <stdint.h>

// Bring up BLE, advertise for `window_ms` (staying up while a client is
// connected, capped by BLE_MAX_CONNECTED_MS), then shut BLE down.
void ble_run(uint32_t window_ms);
