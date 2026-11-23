# TH-Logger — ultra-low-power Seeed XIAO ESP32-C6 + AHT21 temperature/humidity logger

Battery-powered logger that takes a reading every 10 minutes, stores them in flash
(about 4 years' worth, kept through power loss), and serves them over BLE and USB.

The readings are taken by the C6's **LP (low-power) RISC-V core**, which bit-bangs I²C. The
main core and radio stay in deep sleep. They wake when the D0 button is pressed, and about once a day to move the new
readings to flash and serve BLE.

## Wiring

| AHT21 | XIAO ESP32-C6 |
|-------|---------------|
| VIN   | 3V3           |
| GND   | GND           |
| SDA   | D2 (GPIO2)    |
| SCL   | D1 (GPIO1)    |

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

Build settings are in `sdkconfig.defaults` and the flash layout in `partitions.csv`.
After changing either, delete the generated `sdkconfig.esp32c6*` files so the new
settings are picked up.

Note: the main core is asleep almost all the time, so USB disconnects. To re-flash,
press the D0 button while the board is plugged in (it then stays awake), or hold BOOT,
tap RESET, then release BOOT.

## How it works

```
LP core (ulp/main.c)                       HP core (src/)
 every 10 min, woken by LP timer:           wakes on: power-on / button / LP core
  read AHT21 (bit-banged I2C, ~100 ms)          - power-on: load + start LP core
  stage in ring buffer in LP SRAM            - move staged readings to flash log
  every N readings, or when the  ───────▶   - advertise BLE for a window
  ring is nearly full: wake HP core          - serve the log from flash
                                             - deep sleep again
```

- **Storage:** readings are staged in LP SRAM (room for 24 h) and moved to the
  `thlog` flash partition every time the main core wakes: on a button press, on
  any regular BLE wake, and while it stays awake. Otherwise the LP core wakes the
  main core when the staging buffer is nearly full (about once a day), just to
  move the readings. The flash log is a ring of 4 KB sectors holding about 212,000
  readings (about 4 years). When full, the oldest sector (340 readings) is
  overwritten. Readings still in LP SRAM are lost if power is removed: up to about
  a day's worth by default (an hour with hourly BLE wakes).

- **Timestamps:** the LP core records the LP-timer tick count. The main core converts
  it to unix time when it moves the reading to flash. Readings taken before the
  clock is set are stored as seconds since power-on, together with a power-on
  counter. When the clock is first set, the offset is saved in NVS so those
  readings get real timestamps too. If the logger loses power before its clock is
  ever set, that power-on's readings have no known time and are reported with
  timestamp 0.
- **Slot alignment:** once the clock is set, the HP core reprograms the LP timer so
  readings fall on whole 10-minute slots. It corrects drift every time it wakes.
- **BLE availability** (`include/config.h`):
  - By default BLE runs **only after a D0 button press** (or power-on): the device
    advertises for `BLE_WINDOW_BUTTON_MS` (60 s).
  - Optional regular windows: set `BLE_WAKE_EVERY_N_READINGS` (e.g. 6 = once an hour)
    and the device advertises for `BLE_WINDOW_AFTER_WAKE_MS` (30 s) after every N
    readings. Default `0` = off.
  - It stays awake while a client is connected and making requests. It goes back to
    sleep after `BLE_MAX_IDLE_MS` (30 s) without one.

## BLE protocol

Device name `TH-Logger` by default; each unit can be renamed (see Name below, the
`name` USB command, `th_client.py rename`, or *Rename logger…* on the web page). The
name is kept in NVS, so it survives power loss and re-flashing. Service `7e1a0000-5d3b-4c6a-9f2e-1c0a7b3d9e10`.
All values are little-endian.

| Char UUID prefix | Props | Format |
|---|---|---|
| `7e1a0001` Time    | R/W | `uint32` unix time (UTC). Write it to set the clock |
| `7e1a0002` Current | R   | latest record (see below) |
| `7e1a0003` Cursor  | R/W | `uint32` record number to start the next History page at |
| `7e1a0004` History | R   | `uint32 next, uint32 start, uint8 n` + `n` records (max 60 per page) |
| `7e1a0005` Status  | R   | `uint8 flags (bit0=clock set), uint16 interval_s, uint32 first, uint32 next, uint32 capacity, uint32 next_due` |
| `7e1a0006` Control | W   | `uint8` command: `0x01` = delete all readings |
| `7e1a0007` Name    | R/W | advertised name, 1–24 printable ASCII characters (no length byte). Write it to rename; an empty write restores `TH-Logger` |
| `7e1a0008` ID      | R   | 6-byte factory MAC address: a fixed ID for this unit that doesn't change when it is renamed |

**Record** (8 bytes): `uint32 timestamp, int16 temp (0.01 °C), uint16 humidity (0.01 %RH)`.
A failed sensor read is stored as temp `-32768` and humidity `0xFFFF`.

**Record numbers:** the log holds records `first` to `next - 1`, oldest first. Numbers
keep counting up across wrap-around and clears and are never reused, so a client can
remember `next` and later fetch only newer readings. The page client keeps a copy of
what it has read and does this. A clear moves `first` up to `next`.

**Reading the history:** read Status. Write `first` (or the `next` you saw last time)
to Cursor and read History. While `start + n < next`, write `start + n` to Cursor and
read again. A day is 144 readings, which takes 3 reads.

**Timestamps:** unix time (UTC). A value below 1577836800 (2020) means seconds since
power-on: the clock hasn't been set since the logger last lost power. `0` means
unknown (see above). Write a 4-byte unix timestamp to Time to set the clock.

The standard Environmental Sensing Service (0x181A) is also exposed with Temperature
(0x2A6E) and Humidity (0x2A6F), so generic apps such as nRF Connect show the latest values.

## USB access

Plug the logger into a computer and press its D0 button. While a USB host is
connected, the logger stays awake and keeps advertising over BLE. When you unplug
it, it goes back to deep sleep. A charger or power bank does not count as a host.

```sh
pip install pyserial
python tools/th_client.py dump --usb --sync > log.csv   # auto-detects the XIAO
python tools/th_client.py status --usb /dev/ttyACM0
python tools/th_client.py clear --usb                  # asks first; --yes to skip
python tools/th_client.py rename Garage --usb
```

You can also use a serial terminal. Commands are lines of text:

| Command | Reply |
|---|---|
| `status` | `STATUS clock_set=1 now=<unix> interval=600 count=<n> first=<n> next=<n> capacity=<n> next_due=<unix>` |
| `dump [from]` | `ts,temp_c,humidity_pct` header, then one CSV line per reading, oldest first. With `from`, only record numbers ≥ `from` |
| `time <unix>` | sets the clock (UTC) |
| `clear` | deletes all readings |
| `name [new name]` | `NAME <name>`. With a new name (1–24 printable ASCII characters), renames the logger |
| `help` | list of commands |

Every reply ends with `OK` or `ERR <reason>`. In the debug build, log lines
(`I (1234) ...`) may appear in between.

**Opening the port can reset the chip, which loses readings not yet moved to flash.** The C6's USB
serial port resets the chip when RTS is asserted without DTR. Keep both DTR and RTS
asserted (`th_client.py` does), or use a terminal that doesn't toggle them.

### Phone (web page)

`tools/web/index.html` is a single-file Web Bluetooth page. It shows the latest reading,
a chart (24 h, 7 d, 30 d or everything), min/max values and a table. It sets the
logger's clock, exports every reading as CSV, and can clear the logger. It keeps a copy
of what it has read, so after the first time it only fetches new readings and still
shows the data offline. Copies are kept per logger (by its ID), so one phone can read
several loggers; once it holds more than one, a *Showing* list switches between them. To re-read readings that are already on the phone, pick
**last 24 h / 2 days / 7 days / everything again** under *Download* before tapping
Connect. It disconnects as soon as it has read the data, so the logger
can sleep again.

- **Android:** Chrome. **iPhone/iPad:** the free Bluefy browser (Safari has no Web Bluetooth).
- Hosted at **https://vattern.github.io/tempeq/** (deployed from `tools/web` by
  `.github/workflows/pages.yml` on every push to `main`).
- Press the logger's D0 button, then tap **Connect** and pick your logger (TH-Logger
  unless renamed). **Rename logger…** changes the name it advertises as.
- Add `?demo` to the URL to preview the page with made-up data.

### Android app

`android/` is a native Kotlin / Jetpack Compose app (Android 12 or later). It does what the
web page does: chart, min/max, CSV export, rename, clear, several loggers. It also has
**background sync**: once it's switched on, the phone runs a low-power BLE scan filtered on
the logger's service UUID. The system keeps that scan running even when the app is closed.
When a logger starts advertising (press its D0 button), the app connects, sets the clock,
fetches only the new readings and disconnects. Readings are kept in a SQLite database on
the phone.

```sh
cd android
./gradlew assembleDebug          # app/build/outputs/apk/debug/app-debug.apk
./gradlew installDebug           # onto a phone connected with USB debugging
```

It needs the Android SDK (set `sdk.dir` in `android/local.properties` or `ANDROID_HOME`), or
open `android/` in Android Studio. So with background sync on, pressing D0 is enough: the
phone picks the logger up within its 60 s window without opening the app. Some
phone makers' battery savers stop background work anyway; exempting the app from battery
optimisation helps.

**Releases:** pushing a version tag builds a signed APK and publishes it as a GitHub
Release (`.github/workflows/android-release.yml`):

```sh
git tag v0.2 && git push origin v0.2
```

Install it on any Android 12+ phone by downloading the APK from the Releases page (allow
installs from unknown sources), or add the repository URL to
[Obtainium](https://github.com/ImranR98/Obtainium) to get updates automatically. The
version code comes from the tag (`v1.2.3` → 10203), so each tag must be higher than the
last. A phone with a debug build from `installDebug` must uninstall it before installing
a release (the signing keys differ). Uninstalling also deletes the readings stored on that phone.

One-time setup: create a release keystore, keep it and its password safe (updates can only
be signed with the same key), and add it to the repository secrets:

```sh
keytool -genkeypair -v -keystore thlogger-release.jks -alias thlogger \
        -keyalg RSA -keysize 4096 -validity 10000
gh secret set ANDROID_KEYSTORE_BASE64 < <(base64 -w0 thlogger-release.jks)
gh secret set ANDROID_KEYSTORE_PASSWORD
gh secret set ANDROID_KEY_ALIAS --body thlogger
gh secret set ANDROID_KEY_PASSWORD      # same as the keystore password unless you set another
```

### Host tool (Bluetooth)

```sh
pip install bleak
python tools/th_client.py dump --sync > log.csv   # every stored reading
python tools/th_client.py status
python tools/th_client.py clear                    # asks first; --yes to skip
python tools/th_client.py rename Garage            # change the advertised name
python tools/th_client.py status --name Garage     # pick one logger when there are several
```

Without `--name`, the tool uses the first logger it finds.

A full dump over Bluetooth moves 60 readings per round trip, about a month of
readings in 15–30 s. USB is much faster for large logs.

## Power notes

- **Logging cost:** the main core never wakes to take a reading. The LP core runs for
  about 100 ms every 10 minutes, a tiny fraction of the cost of booting the main core.
  Moving readings to flash adds a few milliseconds to each wake.
- **BLE is the main power cost, so by default it only runs after a button press.** The
  main core otherwise wakes only about once a day to move readings to flash, and the
  average current is close to the deep-sleep floor. Regular windows
  (`BLE_WAKE_EVERY_N_READINGS = 6`) with a 30 s window cost about 20–30 mA × 30 s each,
  roughly 5 mAh/day.
- Deep sleep with the LP timer running is around 7–10 µA for the C6 plus about 0.25 µA
  for the AHT21. Dev boards add their own regulator and LED quiescent current, which is
  often far higher. For long battery life, use a bare module or remove the power LED and
  use a low-Iq LDO (for example MCP1700). Measure the XIAO's own sleep current before
  relying on these figures.
- **Clock accuracy:** the XIAO has no 32 kHz crystal, so time comes from the internal
  ~136 kHz RC oscillator, which can drift by minutes per day. Re-sync over BLE regularly
  (`--sync`).
