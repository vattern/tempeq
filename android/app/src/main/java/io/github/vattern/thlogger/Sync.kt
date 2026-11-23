package io.github.vattern.thlogger

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.ParcelUuid
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.roundToLong

data class SyncResult(val id: String, val name: String, val fetched: Int)

/** Everything the app does over BLE. Callers must hold BLUETOOTH_SCAN and BLUETOOTH_CONNECT. */
@SuppressLint("MissingPermission")
object Sync {
    fun adapter(context: Context) = context.getSystemService(BluetoothManager::class.java)?.adapter

    fun serviceFilter(): ScanFilter = ScanFilter.Builder().setServiceUuid(ParcelUuid(Th.SERVICE)).build()

    /** Advertising loggers, as the scan sees them (repeats included). */
    fun scan(context: Context, address: String? = null): Flow<BluetoothDevice> = callbackFlow {
        val scanner = adapter(context)?.bluetoothLeScanner ?: throw GattException("Bluetooth is off")
        val cb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                trySend(result.device)
            }

            override fun onScanFailed(errorCode: Int) {
                close(GattException("scan failed ($errorCode)"))
            }
        }
        val filter = if (address == null) serviceFilter()
        else ScanFilter.Builder().setDeviceAddress(address).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanner.startScan(listOf(filter), settings, cb)
        awaitClose { runCatching { scanner.stopScan(cb) } }
    }

    /** Every distinct logger seen within [timeoutMs]. */
    suspend fun findAll(context: Context, timeoutMs: Long): List<BluetoothDevice> {
        val found = mutableListOf<BluetoothDevice>()
        withTimeoutOrNull(timeoutMs) {
            scan(context).collect { d ->
                if (found.none { it.address == d.address }) found += d
            }
        }
        return found
    }

    /** A specific logger, once it advertises (press its button), or null after [timeoutMs]. */
    suspend fun find(context: Context, address: String, timeoutMs: Long): BluetoothDevice? =
        withTimeoutOrNull(timeoutMs) { scan(context, address).first() }

    /**
     * Sets the logger's clock and copies every reading this phone doesn't have yet.
     * [forceDays] > 0 re-reads that many days even if already stored; [Int.MAX_VALUE] re-reads everything.
     */
    suspend fun syncLogger(
        context: Context,
        device: BluetoothDevice,
        forceDays: Int = 0,
        progress: (String) -> Unit = {},
    ): SyncResult {
        val store = Store.get(context)
        progress("Connecting…")
        GattClient.connect(context, device).use { gatt ->
            val id = if (gatt.has(Th.ID)) idFromMac(gatt.read(Th.ID)) else "dev-${device.address}"

            gatt.write(Th.TIME, u32Bytes(System.currentTimeMillis() / 1000))
            val devTime = parseU32(gatt.read(Th.TIME))
            val nowS = System.currentTimeMillis() / 1000.0
            val status = parseStatus(gatt.read(Th.STATUS))
            val name = (if (gatt.has(Th.NAME)) gatt.read(Th.NAME).decodeToString() else null)
                ?.takeIf { it.isNotEmpty() } ?: device.name ?: Th.DEFAULT_NAME

            val stored = store.logger(id)
            // Record numbers never repeat, so readings already here stay valid even after the
            // logger is cleared or wraps. Only if its numbering went backwards (log partition
            // erased by a re-flash) do the old numbers clash; then start this logger afresh.
            if (stored != null && status.next < stored.next) store.deleteReadings(id)
            val have = stored?.next?.takeIf { it <= status.next }
            var cursor = max(status.first, have ?: status.first)
            if (forceDays == Int.MAX_VALUE) cursor = status.first
            else if (forceDays > 0) {
                val perDay = 86400.0 / (status.intervalS.takeIf { it > 0 } ?: 600)
                cursor = minOf(cursor, max(status.first, status.next - ceil(forceDays * perDay).toLong()))
            }

            // Timestamps from before the clock was set count from power-on: place them on
            // the phone's clock using the logger's current time.
            fun fixTs(ts: Long) =
                if (ts == 0L || ts >= Th.MIN_VALID_EPOCH) ts else (nowS - (devTime - ts)).roundToLong()

            val total = status.next - cursor
            var fetched = 0
            while (cursor < status.next) {
                gatt.write(Th.CURSOR, u32Bytes(cursor))
                val page = parseHistory(gatt.read(Th.HISTORY))
                if (page.start != cursor) throw GattException("log changed while reading; try again")
                if (page.records.isEmpty()) break
                store.insertRecords(id, cursor, page.records.map { it.copy(ts = fixTs(it.ts)) })
                cursor += page.records.size
                fetched += page.records.size
                if (total > 120) progress("Reading log… $fetched of $total")
            }

            store.upsertLogger(
                LoggerInfo(
                    id = id, name = name, address = device.address, next = cursor,
                    intervalS = status.intervalS, capacity = status.capacity, count = 0,
                    lastSync = System.currentTimeMillis(), clockOffset = (devTime - nowS).roundToLong(),
                ),
            )
            return SyncResult(id, name, fetched)
        }
    }

    suspend fun rename(context: Context, device: BluetoothDevice, id: String, name: String): String {
        GattClient.connect(context, device).use { gatt ->
            if (!gatt.has(Th.NAME)) throw GattException("this logger's firmware can't be renamed; update it first")
            gatt.write(Th.NAME, name.encodeToByteArray())
            val newName = gatt.read(Th.NAME).decodeToString().ifEmpty { Th.DEFAULT_NAME }
            Store.get(context).rename(id, newName)
            return newName
        }
    }

    suspend fun clear(context: Context, device: BluetoothDevice) {
        GattClient.connect(context, device).use { gatt ->
            gatt.write(Th.CONTROL, byteArrayOf(Th.CTRL_CLEAR))
        }
    }
}
