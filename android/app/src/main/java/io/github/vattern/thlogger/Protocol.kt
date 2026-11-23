package io.github.vattern.thlogger

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** The logger's GATT protocol (see README "BLE protocol"). All values are little-endian. */
object Th {
    private fun uuid(id: Int) = UUID.fromString("7e1a%04x-5d3b-4c6a-9f2e-1c0a7b3d9e10".format(id))

    val SERVICE: UUID = uuid(0x00)
    val TIME: UUID = uuid(0x01)
    val CURRENT: UUID = uuid(0x02)
    val CURSOR: UUID = uuid(0x03)
    val HISTORY: UUID = uuid(0x04)
    val STATUS: UUID = uuid(0x05)
    val CONTROL: UUID = uuid(0x06)
    val NAME: UUID = uuid(0x07)
    val ID: UUID = uuid(0x08)

    const val CTRL_CLEAR: Byte = 0x01
    const val NAME_MAX_LEN = 24
    const val DEFAULT_NAME = "TH-Logger"

    /** Unix times below this (2020-01-01) count seconds since power-on: clock not set. */
    const val MIN_VALID_EPOCH = 1577836800L
    const val TEMP_ERROR: Int = -32768
    const val HUM_ERROR: Int = 0xFFFF
    const val RECORD_SIZE = 8
}

data class Status(
    val clockSet: Boolean,
    val intervalS: Int,
    val first: Long,
    val next: Long,
    val capacity: Long,
    val nextDue: Long,
)

/** One reading. Temperature and humidity are in hundredths; null = failed sensor read. */
data class Record(val ts: Long, val tempC100: Int?, val hum100: Int?)

data class HistoryPage(val next: Long, val start: Long, val records: List<Record>)

private fun le(bytes: ByteArray): ByteBuffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
private fun ByteBuffer.u32(): Long = int.toLong() and 0xFFFFFFFFL
private fun ByteBuffer.u16(): Int = short.toInt() and 0xFFFF
private fun ByteBuffer.u8(): Int = get().toInt() and 0xFF

fun u32Bytes(v: Long): ByteArray =
    ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v.toInt()).array()

fun parseU32(bytes: ByteArray): Long = le(bytes).u32()

fun parseStatus(bytes: ByteArray): Status {
    val b = le(bytes)
    return Status(
        clockSet = b.u8() and 1 != 0,
        intervalS = b.u16(),
        first = b.u32(),
        next = b.u32(),
        capacity = b.u32(),
        nextDue = b.u32(),
    )
}

private fun ByteBuffer.record(): Record {
    val ts = u32()
    val t = short.toInt()
    val h = u16()
    return Record(ts, t.takeIf { it != Th.TEMP_ERROR }, h.takeIf { it != Th.HUM_ERROR })
}

fun parseRecord(bytes: ByteArray): Record = le(bytes).record()

fun parseHistory(bytes: ByteArray): HistoryPage {
    val b = le(bytes)
    val next = b.u32()
    val start = b.u32()
    val n = b.u8()
    require(bytes.size >= 9 + n * Th.RECORD_SIZE) { "short history page" }
    return HistoryPage(next, start, List(n) { b.record() })
}

/** Stable unit ID, same format as the web page: "mac-" + hex of the factory MAC. */
fun idFromMac(bytes: ByteArray): String = "mac-" + bytes.joinToString("") { "%02x".format(it) }
