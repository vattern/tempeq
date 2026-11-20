#!/usr/bin/env python3
"""Host tool for the TH-Logger: sync the clock and dump the last 24 h of readings.

    pip install bleak
    python tools/th_client.py sync          # set device clock to this PC's UTC time
    python tools/th_client.py dump          # print history as CSV
    python tools/th_client.py dump --sync   # set clock first, then dump
    python tools/th_client.py status

The device only advertises briefly after each reading (or for 60 s after a
button press), so the tool keeps scanning for up to --timeout seconds.
"""
import argparse
import asyncio
import datetime as dt
import struct
import time

from bleak import BleakClient, BleakScanner

BASE = "-5d3b-4c6a-9f2e-1c0a7b3d9e10"
UUID_SERVICE = "7e1a0000" + BASE
UUID_TIME = "7e1a0001" + BASE
UUID_CURRENT = "7e1a0002" + BASE
UUID_CURSOR = "7e1a0003" + BASE
UUID_HISTORY = "7e1a0004" + BASE
UUID_STATUS = "7e1a0005" + BASE
MIN_VALID_EPOCH = 1577836800


def fmt_record(ts, t, h):
    when = (dt.datetime.fromtimestamp(ts, dt.timezone.utc).isoformat()
            if ts >= MIN_VALID_EPOCH else f"+{ts}s-since-boot")
    temp = "" if t == -32768 else f"{t / 100:.2f}"
    hum = "" if h == 0xFFFF else f"{h / 100:.2f}"
    return f"{when},{temp},{hum}"


async def read_history(client):
    records, cursor = [], 0
    while True:
        await client.write_gatt_char(UUID_CURSOR, bytes([cursor]), response=True)
        page = await client.read_gatt_char(UUID_HISTORY)
        total, start, n = page[0], page[1], page[2]
        for i in range(n):
            records.append(struct.unpack_from("<IhH", page, 3 + i * 8))
        cursor = start + n
        if n == 0 or cursor >= total:
            return records


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["sync", "dump", "status"])
    ap.add_argument("--sync", action="store_true", help="also set the clock")
    ap.add_argument("--name", default="TH-Logger")
    ap.add_argument("--timeout", type=float, default=960, help="scan time (s)")
    args = ap.parse_args()

    print(f"Scanning for {args.name} (up to {args.timeout:.0f}s)...")
    dev = await BleakScanner.find_device_by_name(args.name, timeout=args.timeout)
    if dev is None:
        raise SystemExit("Device not found")

    async with BleakClient(dev) as client:
        if args.cmd == "sync" or args.sync:
            now = int(time.time())
            await client.write_gatt_char(UUID_TIME, struct.pack("<I", now), response=True)
            print(f"Clock set to {dt.datetime.fromtimestamp(now, dt.timezone.utc).isoformat()}")

        if args.cmd == "status":
            st = await client.read_gatt_char(UUID_STATUS)
            flags, interval, count, cap, due = struct.unpack("<BHBBI", st)
            dev_time = struct.unpack("<I", await client.read_gatt_char(UUID_TIME))[0]
            cur = struct.unpack("<IhH", await client.read_gatt_char(UUID_CURRENT))
            print(f"clock set: {bool(flags & 1)}  device time: {dev_time}  "
                  f"(host {int(time.time())})")
            print(f"interval: {interval}s  records: {count}/{cap}  next reading: {due}")
            print("latest:", fmt_record(*cur))

        if args.cmd == "dump":
            print("time_utc,temp_c,humidity_pct")
            for rec in await read_history(client):
                print(fmt_record(*rec))


if __name__ == "__main__":
    asyncio.run(main())
