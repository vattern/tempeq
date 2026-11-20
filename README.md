# TH-Logger — ultra-low-power Seeed XIAO ESP32-C6 + AHT21 temperature/humidity logger

Battery-powered logger that takes a reading every 15 minutes, keeps the last
24 hours (96 readings), and serves them over BLE.

The readings are taken by the C6's **LP (low-power) RISC-V core**, which bit-bangs I²C. The
main core and radio stay in deep sleep and only wake to serve BLE.

## Wiring

| AHT21 | XIAO ESP32-C6 |
|-------|---------------|
| VIN   | 3V3           |
| GND   | GND           |
| SDA   | D1 (GPIO1)    |
| SCL   | D2 (GPIO2)    |

Optional wake button: D0 (GPIO0) → button → GND, with a **100 kΩ external pull-up** to 3V3.

**Why these pins:** only LP GPIOs (GPIO0–7) can be used by the LP core or wake the chip
from deep sleep, and the XIAO only breaks out GPIO0–2 on its headers. The C6's hardware
LP-I2C is fixed to GPIO6/7, which the XIAO only exposes on the tiny MTCK/MTDO pads
underneath, so the LP core bit-bangs I²C on D1/D2 instead. Pins are set in
`include/th_shared.h` and `include/config.h`.

The I²C needs pull-ups. The firmware enables the weak internal ones, but the AHT21
breakout's own pull-ups (usually 4.7–10 kΩ) are what make the bus reliable. A bare
AHT21 chip needs external 10 kΩ pull-ups on SDA and SCL.

**RF switch:** the XIAO routes the antenna through an RF switch. GPIO3 LOW powers it, and
GPIO14 selects the on-board antenna (LOW) or the U.FL connector (HIGH). The firmware
switches it on only for BLE and holds it off in deep sleep. For an external antenna, set
`USE_EXTERNAL_ANTENNA 1` in `include/config.h`.

**Battery:** solder a 1-cell LiPo to the BAT+/BAT− pads on the back. The XIAO charges it
over USB.

## Build / flash

ESP-IDF 5.5 via PlatformIO (`framework = espidf`):

```sh
pio run -e esp32c6 -t upload                     # production (warnings-only logging)
pio run -e esp32c6-debug -t upload -t monitor    # with info logging
```

Build settings are in `sdkconfig.defaults`. After changing it, delete the generated
`sdkconfig.esp32c6*` files so the new defaults are picked up.

Note: the main core is asleep almost all the time, so USB disconnects. To re-flash,
hold BOOT, tap RESET, then release BOOT.

## How it works

```
LP core (ulp/main.c)                       HP core (src/)
 every 15 min, woken by LP timer:           wakes on: power-on / button / LP core
  read AHT21 (bit-banged I2C, ~100 ms)          - power-on: load + start LP core
  append to ring buffer in LP SRAM           - advertise BLE for a window
  every N readings: wake HP core  ───────▶   - serve log straight from LP SRAM
                                             - deep sleep again
```

- **Timestamps:** each record stores the LP-timer tick count. The HP core converts
  ticks to unix time whenever it serves data. Setting the clock therefore corrects
  every stored reading, including those taken before the clock was set.
- **Slot alignment:** once the clock is set, the HP core reprograms the LP timer so
  readings fall on :00, :15, :30 and :45. It corrects drift every time it wakes.
- **BLE availability** (`include/config.h`):
  - After every `BLE_WAKE_EVERY_N_READINGS` readings (default 4, so once an hour),
    the device advertises for `BLE_WINDOW_AFTER_WAKE_MS` (5 s).
  - After a button press or power-on, it advertises for `BLE_WINDOW_BUTTON_MS` (60 s).
  - It stays awake while a client is connected, up to `BLE_MAX_CONNECTED_MS`.

The log lives in LP SRAM. It survives deep sleep but is lost if power is removed or
the board is reset.

## BLE protocol

Device name `TH-Logger`. Service `7e1a0000-5d3b-4c6a-9f2e-1c0a7b3d9e10`.
All values are little-endian.

| Char UUID prefix | Props | Format |
|---|---|---|
| `7e1a0001` Time    | R/W | `uint32` unix time (UTC). Write it to set the clock |
| `7e1a0002` Current | R   | latest record (see below) |
| `7e1a0003` Cursor  | R/W | `uint8` start index for the History page (0 = oldest) |
| `7e1a0004` History | R   | `uint8 total, uint8 start, uint8 n` + `n` records (max 60 per page) |
| `7e1a0005` Status  | R   | `uint8 flags (bit0=clock set), uint16 interval_s, uint8 count, uint8 capacity, uint32 next_due` |

**Record** (8 bytes): `uint32 timestamp, int16 temp (0.01 °C), uint16 humidity (0.01 %RH)`.
A failed sensor read is stored as temp `-32768` and humidity `0xFFFF`.

**Reading the 24 h history:** write `0` to Cursor and read History. If `start + n < total`,
write `start + n` to Cursor and read again. A full day takes 2 reads.

**Setting the date:** write a 4-byte unix timestamp to Time. Before the clock is set,
timestamps count seconds since power-on.

The standard Environmental Sensing Service (0x181A) is also exposed with Temperature
(0x2A6E) and Humidity (0x2A6F), so generic apps such as nRF Connect show the latest values.

### Phone (web page)

`tools/web/index.html` is a single-file Web Bluetooth page. It shows the latest reading,
a 24 h chart, min/max values and the full table. It sets the logger's clock and exports
CSV. It disconnects as soon as it has read the data, so the logger can sleep again.
The last download is cached, so the page still shows it offline.

- **Android:** Chrome. **iPhone/iPad:** the free Bluefy browser (Safari has no Web Bluetooth).
- Hosted at **https://vattern.github.io/tempeq/** (deployed from `tools/web` by
  `.github/workflows/pages.yml` on every push to `main`).
- Press the logger's D0 button, then tap **Connect** and pick TH-Logger.
- Add `?demo` to the URL to preview the page with made-up data.

### Host tool

```sh
pip install bleak
python tools/th_client.py dump --sync > log.csv
python tools/th_client.py status
```

## Power notes

- **Logging cost:** the main core never wakes to take a reading. The LP core runs for
  about 100 ms every 15 minutes, a tiny fraction of the old cost of booting the main core.
- **BLE windows are now the main power cost.** A 5 s window costs about 20–30 mA × 5 s.
  At the default of once an hour that is roughly 1 mAh/day. Set
  `BLE_WAKE_EVERY_N_READINGS` to `0` to advertise only after a button press. Then the
  average current is close to the deep-sleep floor.
- Deep sleep with the LP timer running is around 7–10 µA for the C6 plus about 0.25 µA
  for the AHT21. Dev boards add their own regulator and LED quiescent current, which is
  often far higher. For long battery life, use a bare module or remove the power LED and
  use a low-Iq LDO (for example MCP1700). Measure the XIAO's own sleep current before
  relying on these figures.
- **Clock accuracy:** the XIAO has no 32 kHz crystal, so time comes from the internal
  ~136 kHz RC oscillator, which can drift by minutes per day. Re-sync over BLE regularly
  (`--sync`).
