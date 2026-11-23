package io.github.vattern.thlogger

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.Context
import android.os.Build
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.delay
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withTimeout
import java.io.Closeable
import java.util.UUID

class GattException(message: String) : Exception(message)

/**
 * Minimal coroutine wrapper over [BluetoothGatt]: one operation at a time, each awaited.
 * Callers must hold BLUETOOTH_CONNECT.
 */
@SuppressLint("MissingPermission")
class GattClient private constructor() : Closeable {
    private lateinit var gatt: BluetoothGatt
    private val lock = Mutex()
    private var pending: CompletableDeferred<ByteArray>? = null
    private val connected = CompletableDeferred<Unit>()

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                connected.complete(Unit)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                val e = GattException("disconnected (status $status)")
                connected.completeExceptionally(e)
                pending?.completeExceptionally(e)
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) = finish(status, ByteArray(0))
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) = finish(status, ByteArray(0))

        override fun onCharacteristicRead(
            g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int,
        ) = finish(status, value)

        @Deprecated("Used below Android 13")
        override fun onCharacteristicRead(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            @Suppress("DEPRECATION")
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) finish(status, c.value ?: ByteArray(0))
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) =
            finish(status, ByteArray(0))
    }

    private fun finish(status: Int, value: ByteArray) {
        val p = pending ?: return
        if (status == BluetoothGatt.GATT_SUCCESS) p.complete(value)
        else p.completeExceptionally(GattException("GATT error $status"))
    }

    /** Runs one GATT operation: [start] must return true if the request was queued. */
    private suspend fun op(what: String, start: () -> Boolean): ByteArray = lock.withLock {
        val d = CompletableDeferred<ByteArray>()
        pending = d
        try {
            if (!start()) throw GattException("$what failed to start")
            withTimeout(OP_TIMEOUT_MS) { d.await() }
        } finally {
            pending = null
        }
    }

    private fun characteristic(uuid: UUID): BluetoothGattCharacteristic? =
        gatt.getService(Th.SERVICE)?.getCharacteristic(uuid)

    fun has(uuid: UUID) = characteristic(uuid) != null

    suspend fun read(uuid: UUID): ByteArray {
        val c = characteristic(uuid) ?: throw GattException("characteristic $uuid missing")
        return op("read") { gatt.readCharacteristic(c) }
    }

    suspend fun write(uuid: UUID, value: ByteArray) {
        val c = characteristic(uuid) ?: throw GattException("characteristic $uuid missing")
        val type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        op("write") {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeCharacteristic(c, value, type) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                c.writeType = type
                @Suppress("DEPRECATION")
                c.value = value
                @Suppress("DEPRECATION")
                gatt.writeCharacteristic(c)
            }
        }
    }

    override fun close() {
        try {
            gatt.disconnect()
        } catch (_: Exception) {
        }
        gatt.close()
    }

    companion object {
        private const val OP_TIMEOUT_MS = 10_000L
        private const val CONNECT_TIMEOUT_MS = 15_000L

        /** Connects, raises the MTU (a history page is 489 bytes) and discovers services. */
        suspend fun connect(context: Context, device: BluetoothDevice, attempts: Int = 3): GattClient {
            var last: Exception? = null
            repeat(attempts) { attempt ->
                if (attempt > 0) delay(500)
                val client = GattClient()
                try {
                    client.gatt = device.connectGatt(context, false, client.callback, BluetoothDevice.TRANSPORT_LE)
                    withTimeout(CONNECT_TIMEOUT_MS) { client.connected.await() }
                    client.op("MTU request") { client.gatt.requestMtu(517) }
                    client.op("service discovery") { client.gatt.discoverServices() }
                    if (client.gatt.getService(Th.SERVICE) == null) throw GattException("not a TH-Logger")
                    return client
                } catch (e: Exception) {
                    // Status 133 and friends are common on Android; a fresh attempt usually works.
                    last = e
                    if (client::gatt.isInitialized) client.close()
                }
            }
            throw last ?: GattException("connect failed")
        }
    }
}
