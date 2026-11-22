#!/usr/bin/env python3
"""Host tool for the TH-Logger: sync the clock, dump all stored readings, clear the log.

Over Bluetooth (needs: pip install bleak):
    python tools/th_client.py sync          # set device clock to this PC's UTC time
    python tools/th_client.py dump          # print every stored reading as CSV
    python tools/th_client.py dump --sync   # set clock first, then dump
    python tools/th_client.py status
    python tools/th_client.py clear         # delete all readings (asks first; --yes to skip)

Over USB (needs: pip install pyserial):
    python tools/th_client.py dump --usb            # auto-detect the XIAO
    python tools/th_client.py status --usb /dev/ttyACM0

The logger sleeps most of the time. Press its button to wake it. Over Bluetooth
it advertises briefly; over USB it stays awake for as long as it is plugged in.
The tool keeps waiting for up to --timeout seconds.
"""
import argparse
import asyncio
import datetime as dt
import re
import struct
import sys
import time

BASE = "-5d3b-4c6a-9f2e-1c0a7b3d9e10"
UUID_SERVICE = "7e1a0000" + BASE
UUID_TIME = "7e1a0001" + BASE
UUID_CURRENT = "7e1a0002" + BASE
UUID_CURSOR = "7e1a0003" + BASE
UUID_HISTORY = "7e1a0004" + BASE
UUID_STATUS = "7e1a0005" + BASE
UUID_CONTROL = "7e1a0006" + BASE
CTRL_CLEAR = 0x01
MIN_VALID_EPOCH = 1577836800

USB_VID, USB_PID = 0x303A, 0x1001  # Espressif USB-Serial/JTAG
LOG_LINE = re.compile(r"^[EWIDV] \(\d+\)")


def fmt_time(ts):
    if ts == 0:
        return ""  # taken during an earlier power-on whose clock was never set
    if ts >= MIN_VALID_EPOCH:
        return dt.datetime.fromtimestamp(ts, dt.timezone.utc).isoformat()
    return f"+{ts}s-since-boot"


def fmt_record(ts, t, h):
    temp = "" if t is None else f"{t:.2f}"
    hum = "" if h is None else f"{h:.2f}"
    return f"{fmt_time(ts)},{temp},{hum}"


def print_status(clock_set, dev_time, interval, count, cap, due, latest):
    print(f"clock set: {clock_set}  device time: {dev_time}  (host {int(time.time())})")
    days = count * interval / 86400
    print(f"interval: {interval}s  readings: {count} ({days:.1f} days) of {cap}  next reading: {fmt_time(due)}")
    if latest:
        print("latest:", fmt_record(*latest))


# ---------------------------------------------------------------------------
# Bluetooth
# ---------------------------------------------------------------------------
def decode_record(buf, off=0):
    ts, t, h = struct.unpack_from("<IhH", buf, off)
    return ts, (None if t == -32768 else t / 100), (None if h == 0xFFFF else h / 100)


async def ble_status(client):
    flags, interval, first, nxt, cap, due = struct.unpack("<BHIIII", await client.read_gatt_char(UUID_STATUS))
    return dict(clock_set=bool(flags & 1), interval=interval, first=first, next=nxt, cap=cap, due=due)


async def ble_read_history(client, first):
    """Read every record from record number `first` on (60 per round trip)."""
    records, cursor = [], first
    while True:
        await client.write_gatt_char(UUID_CURSOR, struct.pack("<I", cursor), response=True)
        page = await client.read_gatt_char(UUID_HISTORY)
        nxt, start, n = struct.unpack_from("<IIB", page)
        records += [decode_record(page, 9 + i * 8) for i in range(n)]
        cursor = start + n
        if n == 0 or cursor >= nxt:
            return records
        if len(records) % 1200 == 0:
            print(f"  {len(records)} / {nxt - first} readings...", file=sys.stderr)


def confirm_clear(args, count):
    if args.yes:
        return True
    if not sys.stdin.isatty():
        raise SystemExit("Refusing to clear without confirmation: pass --yes")
    answer = input(f"Delete all {count} readings stored on the logger? This cannot be undone. [y/N] ")
    return answer.strip().lower() in ("y", "yes")


async def run_ble(args):
    from bleak import BleakClient, BleakScanner

    print(f"Scanning for {args.name} (up to {args.timeout:.0f}s)...", file=sys.stderr)
    dev = await BleakScanner.find_device_by_name(args.name, timeout=args.timeout)
    if dev is None:
        raise SystemExit("Device not found")

    async with BleakClient(dev) as client:
        if args.cmd == "sync" or args.sync:
            now = int(time.time())
            await client.write_gatt_char(UUID_TIME, struct.pack("<I", now), response=True)
            print(f"Clock set to {fmt_time(now)}", file=sys.stderr)

        st = await ble_status(client)
        count = st["next"] - st["first"]

        if args.cmd == "status":
            dev_time = struct.unpack("<I", await client.read_gatt_char(UUID_TIME))[0]
            latest = decode_record(await client.read_gatt_char(UUID_CURRENT)) if count else None
            print_status(st["clock_set"], dev_time, st["interval"], count, st["cap"], st["due"], latest)

        if args.cmd == "dump":
            print(f"Reading {count} readings...", file=sys.stderr)
            print("time_utc,temp_c,humidity_pct")
            for rec in await ble_read_history(client, st["first"]):
                print(fmt_record(*rec))

        if args.cmd == "clear":
            if confirm_clear(args, count):
                await client.write_gatt_char(UUID_CONTROL, bytes([CTRL_CLEAR]), response=True)
                print("Log cleared", file=sys.stderr)


# ---------------------------------------------------------------------------
# USB
# ---------------------------------------------------------------------------
def find_usb_port():
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == USB_VID and p.pid == USB_PID:
            return p.device
    return None


def open_usb(port_arg, timeout):
    import serial

    deadline = time.time() + timeout
    announced = False
    while True:
        port = find_usb_port() if port_arg == "auto" else port_arg
        if port:
            try:
                # Open with DTR and RTS both asserted. On the ESP32 USB-Serial/JTAG
                # port, RTS without DTR resets the chip, which would wipe the log.
                s = serial.Serial()
                s.port, s.baudrate, s.timeout = port, 115200, 0.5
                s.dtr, s.rts = True, True
                s.open()
                return s
            except (serial.SerialException, OSError):
                pass
        if time.time() > deadline:
            raise SystemExit("Logger not found on USB. Is it plugged in? Press its button to wake it.")
        if not announced:
            print(f"Waiting for the logger on USB (up to {timeout:.0f}s): press its button to wake it...",
                  file=sys.stderr)
            announced = True
        time.sleep(0.5)


def usb_command(s, cmd, timeout=5.0):
    """Send a command; return the reply lines before OK. Ignores log output.
    `timeout` is the longest gap allowed between reply lines."""
    s.reset_input_buffer()
    s.write(b"\n" + cmd.encode() + b"\n")
    lines, deadline = [], time.time() + timeout
    while time.time() < deadline:
        raw = s.readline()
        if not raw:
            continue
        deadline = time.time() + timeout
        line = raw.decode(errors="replace").strip()
        if not line or LOG_LINE.match(line):
            continue
        if line == "OK":
            return lines
        if line.startswith("ERR"):
            raise SystemExit(f"Logger replied: {line}")
        lines.append(line)
    raise SystemExit(f"No reply to '{cmd}' over USB")


def run_usb(args):
    s = open_usb(args.usb, args.timeout)
    # The board needs a moment after waking/opening before it answers.
    for attempt in range(10):
        try:
            usb_command(s, "help", timeout=1.0)
            break
        except SystemExit:
            if attempt == 9:
                raise
    with s:
        if args.cmd == "sync" or args.sync:
            now = int(time.time())
            usb_command(s, f"time {now}")
            print(f"Clock set to {fmt_time(now)}", file=sys.stderr)

        line = next((l for l in usb_command(s, "status") if l.startswith("STATUS ")), None)
        if not line:
            raise SystemExit("Unexpected status reply")
        f = dict(kv.split("=", 1) for kv in line.split()[1:])
        count = int(f["count"])

        if args.cmd == "status":
            # Only the latest reading is needed: dump from the last record number.
            rows = parse_dump(usb_command(s, f"dump {max(int(f['next']) - 1, 0)}"))
            print_status(f["clock_set"] == "1", int(f["now"]), int(f["interval"]), count,
                         int(f["capacity"]), int(f["next_due"]), rows[-1] if rows else None)

        if args.cmd == "dump":
            print("time_utc,temp_c,humidity_pct")
            for rec in parse_dump(usb_command(s, "dump")):
                print(fmt_record(*rec))

        if args.cmd == "clear":
            if confirm_clear(args, count):
                usb_command(s, "clear")
                print("Log cleared", file=sys.stderr)


def parse_dump(lines):
    rows = []
    for line in lines:
        parts = line.split(",")
        if len(parts) != 3 or not parts[0].isdigit():
            continue  # header or stray output
        try:
            rows.append((int(parts[0]),
                         float(parts[1]) if parts[1] else None,
                         float(parts[2]) if parts[2] else None))
        except ValueError:
            continue  # line mangled by interleaved log output
    return rows


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["sync", "dump", "status", "clear"])
    ap.add_argument("--sync", action="store_true", help="also set the clock")
    ap.add_argument("--yes", action="store_true", help="clear without asking")
    ap.add_argument("--usb", nargs="?", const="auto", metavar="PORT",
                    help="use USB instead of Bluetooth (port auto-detected if omitted)")
    ap.add_argument("--name", default="TH-Logger", help="Bluetooth device name")
    ap.add_argument("--timeout", type=float, default=960, help="how long to wait for the logger (s)")
    args = ap.parse_args()

    if args.usb:
        run_usb(args)
    else:
        asyncio.run(run_ble(args))


if __name__ == "__main__":
    main()
