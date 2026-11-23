#include "ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
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
static const ble_uuid128_t uuid_cursor = TH_UUID(0x03);    // R/W uint32 history start record number
static const ble_uuid128_t uuid_history = TH_UUID(0x04);   // R   page of records
static const ble_uuid128_t uuid_status = TH_UUID(0x05);    // R   flags/interval/range/...
static const ble_uuid128_t uuid_control = TH_UUID(0x06);   // W   commands (0x01 = clear log)
static const ble_uuid128_t uuid_name = TH_UUID(0x07);      // R/W advertised device name
static const ble_uuid128_t uuid_id = TH_UUID(0x08);        // R   6-byte factory MAC (unit ID)

#define CTRL_CLEAR 0x01

enum { CH_TIME, CH_CURRENT, CH_CURSOR, CH_HISTORY, CH_STATUS, CH_CONTROL, CH_NAME, CH_ID, CH_ESS_TEMP, CH_ESS_HUM };

static volatile bool connected;
static volatile bool synced;
static volatile bool adv_wanted;
static volatile int64_t last_activity_us;
static uint16_t conn_handle;
static uint8_t own_addr_type;
static uint32_t hist_cursor;

static char dev_name[BLE_NAME_MAX_LEN + 1];

static void advertise(void);

// ---------------------------------------------------------------------------
// Device name (kept in NVS, namespace "thcfg", key "name")
// ---------------------------------------------------------------------------
const char *ble_name(void) {
  if (dev_name[0]) return dev_name;
  strcpy(dev_name, BLE_DEVICE_NAME);
  nvs_handle_t h;
  if (nvs_open("thcfg", NVS_READONLY, &h) == ESP_OK) {
    char buf[sizeof(dev_name)];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, "name", buf, &len) == ESP_OK && buf[0]) strcpy(dev_name, buf);
    nvs_close(h);
  }
  return dev_name;
}

bool ble_set_name(const char *name, size_t len) {
  if (len == 0) {  // back to the default
    name = BLE_DEVICE_NAME;
    len = strlen(name);
  }
  if (len > BLE_NAME_MAX_LEN || name[0] == ' ' || name[len - 1] == ' ') return false;
  for (size_t i = 0; i < len; i++)
    if (name[i] < 0x20 || name[i] > 0x7e) return false;  // printable ASCII only

  nvs_handle_t h;
  if (nvs_open("thcfg", NVS_READWRITE, &h) != ESP_OK) return false;
  char buf[BLE_NAME_MAX_LEN + 1];
  memcpy(buf, name, len);
  buf[len] = '\0';
  bool ok = nvs_set_str(h, "name", buf) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) return false;

  strcpy(dev_name, buf);
  if (synced) {
    ble_svc_gap_device_name_set(dev_name);
    // Re-advertise under the new name now; a connected client sees it after
    // disconnecting (advertising restarts then).
    if (adv_wanted && !connected) {
      ble_gap_adv_stop();
      advertise();
    }
  }
  return true;
}

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
  int ch = (int)(intptr_t)arg;
  last_activity_us = esp_timer_get_time();

  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t buf[BLE_NAME_MAX_LEN];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ch == CH_NAME) {
      return ble_set_name((const char *)buf, len) ? 0 : BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    if (ch == CH_TIME) {
      if (len != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      datalog_set_time(buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24));
      return 0;
    }
    if (ch == CH_CURSOR) {
      if (len != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      hist_cursor = buf[0] | (buf[1] << 8) | (buf[2] << 16) | ((uint32_t)buf[3] << 24);
      return 0;
    }
    if (ch == CH_CONTROL) {
      if (len != 1 || buf[0] != CTRL_CLEAR) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
      datalog_clear();
      return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
  }

  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;

  if (ch == CH_NAME) {
    const char *name = ble_name();
    return append(ctxt->om, name, strlen(name));
  }
  if (ch == CH_ID) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    return append(ctxt->om, mac, sizeof(mac));
  }

  datalog_flush();
  reading_t last;
  const reading_t *latest = datalog_latest(&last) ? &last : NULL;

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
    case CH_CURSOR: {
      uint8_t buf[4];
      put32(buf, hist_cursor);
      return append(ctxt->om, buf, sizeof(buf));
    }
    case CH_HISTORY: {
      // [next u32][start u32][n u8] + n * 8-byte records, oldest first.
      // Long reads arrive as several calls with increasing offsets; build the
      // page once and serve every chunk of the same read from it.
      static uint8_t buf[9 + HISTORY_PAGE_SIZE * 8];
      static uint16_t len;
      if (ctxt->offset == 0 || len == 0) {
        static reading_t page[HISTORY_PAGE_SIZE];
        uint32_t first, next, start;
        datalog_range(&first, &next);
        uint16_t n = datalog_read(hist_cursor, page, HISTORY_PAGE_SIZE, &start);
        put32(buf, next);
        put32(buf + 4, start);
        buf[8] = (uint8_t)n;
        for (int i = 0; i < n; i++) encode_reading(buf + 9 + i * 8, &page[i]);
        len = 9 + n * 8;
      }
      return append(ctxt->om, buf, len);
    }
    case CH_STATUS: {
      // [flags u8 (bit0 = clock set)][interval_s u16][first u32][next u32][capacity u32][next_due u32]
      uint8_t buf[19];
      uint32_t first, next;
      datalog_range(&first, &next);
      buf[0] = datalog_time_valid() ? 0x01 : 0x00;
      put16(buf + 1, TH_LOG_INTERVAL_S);
      put32(buf + 3, first);
      put32(buf + 7, next);
      put32(buf + 11, datalog_capacity());
      put32(buf + 15, datalog_next_due(latest));
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
            CHR(&uuid_control, CH_CONTROL, BLE_GATT_CHR_F_WRITE),
            CHR(&uuid_name, CH_NAME, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE),
            CHR(&uuid_id, CH_ID, BLE_GATT_CHR_F_READ),
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
        last_activity_us = esp_timer_get_time();
        connected = true;
        ESP_LOGI(TAG, "Connected");
      } else {
        advertise();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      connected = false;
      ESP_LOGI(TAG, "Disconnected");
      if (adv_wanted) advertise();
      break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (!connected && adv_wanted) advertise();
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
  const char *name = ble_name();
  rsp.name = (const uint8_t *)name;
  rsp.name_len = strlen(name);
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
  synced = true;
  if (adv_wanted) advertise();
}

static void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
void ble_start(void) {
  connected = false;
  synced = false;
  adv_wanted = true;
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
  ble_svc_gap_device_name_set(ble_name());
  nimble_port_freertos_init(host_task);
}

void ble_set_advertising(bool on) {
  if (on == adv_wanted) return;
  adv_wanted = on;
  if (!synced) return;
  if (on && !connected) advertise();
  if (!on) ble_gap_adv_stop();
}

int64_t ble_idle_us(void) {
  return connected ? esp_timer_get_time() - last_activity_us : -1;
}

void ble_stop(void) {
  adv_wanted = false;  // stop the GAP handler from re-advertising
  ble_gap_adv_stop();
  if (connected) {
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  nimble_port_stop();
  nimble_port_deinit();
}
