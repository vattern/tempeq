package io.github.vattern.thlogger

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.OutOfQuotaPolicy
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import androidx.work.workDataOf
import java.util.concurrent.TimeUnit

private const val TAG = "THBackground"

/**
 * Background sync: a filtered, low-power BLE scan that the system keeps running even when the
 * app isn't. When a logger advertises (after a D0 button press), the system wakes [ScanReceiver],
 * which queues a [SyncWorker] to connect and fetch the new readings.
 */
@SuppressLint("MissingPermission")
object BackgroundSync {
    private const val PREFS = "settings"
    private const val KEY_ENABLED = "background_sync"
    private const val RESCAN_WORK = "rescan"

    /** Don't reconnect to a logger synced more recently than this (one window = one sync). */
    const val MIN_GAP_MS = 2 * 60 * 1000L

    private fun prefs(context: Context) = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun isEnabled(context: Context) = prefs(context).getBoolean(KEY_ENABLED, false)

    fun hasPermissions(context: Context) = listOf(
        Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT,
    ).all { ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED }

    fun setEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit { putBoolean(KEY_ENABLED, enabled) }
        val wm = WorkManager.getInstance(context)
        if (enabled) {
            restart(context)
            // The system drops the scan when Bluetooth is switched off; re-register it now and then.
            wm.enqueueUniquePeriodicWork(
                RESCAN_WORK, ExistingPeriodicWorkPolicy.KEEP,
                PeriodicWorkRequestBuilder<RescanWorker>(6, TimeUnit.HOURS).build(),
            )
        } else {
            stop(context)
            wm.cancelUniqueWork(RESCAN_WORK)
        }
    }

    private fun pendingIntent(context: Context): PendingIntent = PendingIntent.getBroadcast(
        context, 0, Intent(context, ScanReceiver::class.java),
        // Mutable: the system adds the scan results as extras.
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE,
    )

    private fun scanner(context: Context): BluetoothLeScanner? = Sync.adapter(context)?.bluetoothLeScanner

    /** (Re-)registers the background scan if it's enabled and allowed. */
    fun restart(context: Context) {
        if (!isEnabled(context) || !hasPermissions(context)) return
        val scanner = scanner(context) ?: return  // Bluetooth off
        val pi = pendingIntent(context)
        runCatching { scanner.stopScan(pi) }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_POWER)
            .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
            .build()
        val rc = scanner.startScan(listOf(Sync.serviceFilter()), settings, pi)
        Log.i(TAG, "background scan started: $rc")
    }

    private fun stop(context: Context) {
        if (!hasPermissions(context)) return
        runCatching { scanner(context)?.stopScan(pendingIntent(context)) }
    }

    fun onLoggerSeen(context: Context, address: String) {
        val last = Store.get(context).loggerByAddress(address)?.lastSync ?: 0
        if (System.currentTimeMillis() - last < MIN_GAP_MS) return
        WorkManager.getInstance(context).enqueueUniqueWork(
            "sync-$address", ExistingWorkPolicy.KEEP,
            OneTimeWorkRequestBuilder<SyncWorker>()
                .setInputData(workDataOf(SyncWorker.KEY_ADDRESS to address))
                .setExpedited(OutOfQuotaPolicy.RUN_AS_NON_EXPEDITED_WORK_REQUEST)
                .build(),
        )
    }
}

class ScanReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val error = intent.getIntExtra(BluetoothLeScanner.EXTRA_ERROR_CODE, 0)
        if (error != 0) {
            Log.w(TAG, "background scan error $error")
            return
        }
        val results: List<ScanResult> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableArrayListExtra(BluetoothLeScanner.EXTRA_LIST_SCAN_RESULT, ScanResult::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableArrayListExtra(BluetoothLeScanner.EXTRA_LIST_SCAN_RESULT)
        } ?: return
        results.map { it.device.address }.distinct().forEach { BackgroundSync.onLoggerSeen(context, it) }
    }
}

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED || intent.action == Intent.ACTION_MY_PACKAGE_REPLACED) {
            BackgroundSync.restart(context)
        }
    }
}

class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    @SuppressLint("MissingPermission")
    override suspend fun doWork(): Result {
        val address = inputData.getString(KEY_ADDRESS) ?: return Result.failure()
        if (!BackgroundSync.hasPermissions(applicationContext)) return Result.failure()
        val device = Sync.adapter(applicationContext)?.getRemoteDevice(address) ?: return Result.failure()
        return try {
            val r = Sync.syncLogger(applicationContext, device)
            Log.i(TAG, "synced ${r.name}: ${r.fetched} new readings")
            Result.success()
        } catch (e: Exception) {
            // No retry: the advertising window has most likely closed.
            Log.w(TAG, "sync of $address failed", e)
            Result.failure()
        }
    }

    companion object {
        const val KEY_ADDRESS = "address"
    }
}

class RescanWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        BackgroundSync.restart(applicationContext)
        return Result.success()
    }
}
