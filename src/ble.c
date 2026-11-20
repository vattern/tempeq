#include "ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "config.h"
#include "datalog.h"

static const char *TAG = "ble";

// 7e1a00XX-5d3b-4c6a-9f2e-1c0a7b3d9e10 (little-endian byte order)
#define TH_UUID(id) \
  BLE_UUID128_INIT(0x10, 0x9e, 0x3d, 0x7b, 0x0a, 0x1c, 0x2e, 0x9f, 0x6a, 0x4c, 0x3b, 0x5d, id, 0x00, 0x1a, 0x7e)

static const ble_uuid128_t uuid_service = TH_UUID(0x00);
static const ble_uuid128_t uuid_time = TH_UUID(0x01);      // R/W uint32 unix time
static const ble_uuid128_t uuid_current = TH_UUID(0x02);   // R   latest record
static const ble_uuid128_t uuid_cursor = TH_UUID(0x03);    // R/W uint8 history start index
static const ble_uuid128_t uuid_history = TH_UUID(0x04);   // R   page of records
static const ble_uuid128_t uuid_status = TH_UUID(0x05);    // R   flags/interval/count/...

enum { CH_TIME, CH_CURRENT, CH_CURSOR, CH_HISTORY, CH_STATUS, CH_ESS_TEMP, CH_ESS_HUM };

static volatile bool connected;
static volatile int64_t connected_at_us;
static volatile int64_t window_end_us;
static uint16_t conn_handle;
static uint8_t own_addr_type;
static uint8_t hist_cursor;

static void advertise(void);

// ---------------------------------------------------------------------------
// GATT
// ---------------------------------------------------------------------------
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void encode_reading(uint8_t *p, const reading_t *r) {
  put32(p, r->ts);
  put16(p + 4, (uint16_t)r->temp);
  put16(p + 6, r->hum);
}

static int append(struct os_mbuf *om, const void *data, size_t len) {
  return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
  static reading_t log[TH_LOG_CAPACITY];
  int ch = (int)(intptr_t)arg;

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t buf[4];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ch == CH_TIME) {
      if (len != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      datalog_set_time(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24));
      return 0;
    }
    if (ch == CH_CURSOR) {
      if (len != 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      hist_cursor = buf[0];
      return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

  uint16_t count = datalog_snapshot(log);
  const reading_t *latest = count ? &log[count - 1] : NULL;

  switch (ch) {
    case CH_TIME: {
      uint8_t buf[4];
      put32(buf, datalog_now());
      return append(ctxt->om, buf, sizeof(buf));
    }
    case CH_CURRENT: {
      uint8_t buf[8] = {0};
      if (latest) encode_reading(buf, latest);
      return append(ctxt->om, buf, sizeof(buf));
    }
    case CH_CURSOR:
      return append(ctxt->om, &hist_cursor, 1);
    case CH_HISTORY: {
      // [total u8][start u8][n u8] + n * 8-byte records, oldest first.
      static uint8_t buf[3 + HISTORY_PAGE_SIZE * 8];
      uint8_t start = hist_cursor > count ? count : hist_cursor;
      uint8_t n = count - start > HISTORY_PAGE_SIZE ? HISTORY_PAGE_SIZE : count - start;
      buf[0] = count;
      buf[1] = start;
      buf[2] = n;
      for (int i = 0; i < n; i++) encode_reading(buf + 3 + i * 8, &log[start + i]);
      return append(ctxt->om, buf, 3 + n * 8);
    }
    case CH_STATUS: {
      // [flags u8 (bit0 = clock set)][interval_s u16][count u8][capacity u8][next_due u32]
      uint8_t buf[9];
      uint32_t interval = TH_LOG_INTERVAL_S;
      uint32_t now = datalog_now();
      uint32_t next = datalog_time_valid() ? (now / interval + 1) * interval
                                           : (latest ? latest->ts + interval : now + interval);
      buf[0] = datalog_time_valid() ? 0x01 : 0x00;
      put16(buf + 1, interval);
      buf[3] = count;
      buf[4] = TH_LOG_CAPACITY;
      put32(buf + 5, next);
      return append(ctxt->om, buf, sizeof(buf));
    }
    case CH_ESS_TEMP: {  // org.bluetooth.characteristic.temperature, sint16 0.01 degC
      uint8_t buf[2] = {0};
      if (latest) put16(buf, (uint16_t)latest->temp);
      return append(ctxt->om, buf, sizeof(buf));
    }
    case CH_ESS_HUM: {  // org.bluetooth.characteristic.humidity, uint16 0.01 %
      uint8_t buf[2] = {0};
      if (latest) put16(buf, latest->hum);
      return append(ctxt->om, buf, sizeof(buf));
    }
  }
  return BLE_ATT_ERR_UNLIKELY;
}

#define CHR(uuid_ptr, id, fl) \
  { .uuid = (const ble_uuid_t *)(uuid_ptr), .access_cb = gatt_access, .arg = (void *)(intptr_t)(id), .flags = (fl) }

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_service.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            CHR(&uuid_time, CH_TIME, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE),
            CHR(&uuid_current, CH_CURRENT, BLE_GATT_CHR_F_READ),
            CHR(&uuid_cursor, CH_CURSOR, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE),
            CHR(&uuid_history, CH_HISTORY, BLE_GATT_CHR_F_READ),
            CHR(&uuid_status, CH_STATUS, BLE_GATT_CHR_F_READ),
            {0},
        },
    },
    {
        // Standard Environmental Sensing Service, so generic apps show the latest values.
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x181A),
        .characteristics = (struct ble_gatt_chr_def[]){
            CHR(BLE_UUID16_DECLARE(0x2A6E), CH_ESS_TEMP, BLE_GATT_CHR_F_READ),
            CHR(BLE_UUID16_DECLARE(0x2A6F), CH_ESS_HUM, BLE_GATT_CHR_F_READ),
            {0},
        },
    },
    {0},
};

// ---------------------------------------------------------------------------
// GAP
// ---------------------------------------------------------------------------
static int gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        conn_handle = event->connect.conn_handle;
        connected_at_us = esp_timer_get_time();
        connected = true;
        ESP_LOGI(TAG, "Connected");
      } else {
        advertise();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connected = false;
      ESP_LOGI(TAG, "Disconnected");
      if (esp_timer_get_time() < window_end_us) advertise();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (!connected && esp_timer_get_time() < window_end_us) advertise();
      break;
  }
  return 0;
}

static void advertise(void) {
  // Advertising packet: flags + 128-bit service UUID. Scan response: name.
  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &uuid_service;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (ble_gap_adv_set_fields(&fields) != 0) return;

  struct ble_hs_adv_fields rsp = {0};
  rsp.name = (const uint8_t *)BLE_DEVICE_NAME;
  rsp.name_len = strlen(BLE_DEVICE_NAME);
  rsp.name_is_complete = 1;
  if (ble_gap_adv_rsp_set_fields(&rsp) != 0) return;

  struct ble_gap_adv_params params = {
      .conn_mode = BLE_GAP_CONN_MODE_UND,
      .disc_mode = BLE_GAP_DISC_MODE_GEN,
      .itvl_min = BLE_ADV_INTERVAL,
      .itvl_max = BLE_ADV_INTERVAL + 32,
  };
  int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
  if (rc != 0) ESP_LOGW(TAG, "adv start failed: %d", rc);
}

static void on_sync(void) {
  ble_hs_util_ensure_addr(0);
  ble_hs_id_infer_auto(0, &own_addr_type);
  advertise();
}

static void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
void ble_run(uint32_t window_ms) {
  int64_t start = esp_timer_get_time();
  window_end_us = start + (int64_t)window_ms * 1000;
  connected = false;
  hist_cursor = 0;

  if (nimble_port_init() != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed");
    return;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_gatts_count_cfg(gatt_svcs);
  ble_gatts_add_svcs(gatt_svcs);
  ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
  nimble_port_freertos_init(host_task);
  ESP_LOGI(TAG, "Advertising for %lu ms", (unsigned long)window_ms);

  for (;;) {
    int64_t now = esp_timer_get_time();
    if (connected) {
      if (now - connected_at_us > (int64_t)BLE_MAX_CONNECTED_MS * 1000) break;
    } else if (now >= window_end_us) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  window_end_us = 0;  // stop the GAP handler from re-advertising
  ble_gap_adv_stop();
  if (connected) {
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  nimble_port_stop();
  nimble_port_deinit();
}
