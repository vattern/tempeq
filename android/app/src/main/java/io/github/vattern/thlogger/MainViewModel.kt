package io.github.vattern.thlogger

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.util.Locale

enum class Range(val label: String, val seconds: Long?) {
    DAY("24 h", 86_400), WEEK("7 d", 7 * 86_400), MONTH("30 d", 30 * 86_400), ALL("All", null)
}

data class UiState(
    val loggers: List<LoggerInfo> = emptyList(),
    val selectedId: String? = null,
    val range: Range = Range.DAY,
    val readings: List<Reading> = emptyList(),
    val latest: Reading? = null,
    val busy: Boolean = false,
    val message: String? = null,
    val error: Boolean = false,
    val backgroundSync: Boolean = false,
) {
    val selected: LoggerInfo? get() = loggers.firstOrNull { it.id == selectedId }
}

class MainViewModel(app: Application) : AndroidViewModel(app) {
    private val store = Store.get(app)
    private val prefs = app.getSharedPreferences("ui", 0)
    private val _state = MutableStateFlow(
        UiState(
            selectedId = prefs.getString("selected", null),
            range = Range.entries.getOrElse(prefs.getInt("range", 0)) { Range.DAY },
            backgroundSync = BackgroundSync.isEnabled(app),
        ),
    )
    val state: StateFlow<UiState> = _state

    init {
        viewModelScope.launch { store.changes.collect { reload() } }
    }

    private suspend fun reload() {
        val s = _state.value
        val (loggers, id, readings, latest) = withContext(Dispatchers.IO) {
            val loggers = store.loggers()
            val id = s.selectedId?.takeIf { sel -> loggers.any { it.id == sel } } ?: loggers.firstOrNull()?.id
            val since = s.range.seconds?.let { Instant.now().epochSecond - it } ?: 1
            val readings = id?.let { store.readings(it, since) }.orEmpty()
            val latest = readings.lastOrNull() ?: id?.let { store.readings(it).lastOrNull() }
            Quad(loggers, id, readings, latest)
        }
        _state.update { it.copy(loggers = loggers, selectedId = id, readings = readings, latest = latest) }
    }

    private data class Quad<A, B, C, D>(val a: A, val b: B, val c: C, val d: D)

    fun select(id: String) {
        prefs.edit().putString("selected", id).apply()
        _state.update { it.copy(selectedId = id) }
        viewModelScope.launch { reload() }
    }

    fun setRange(range: Range) {
        prefs.edit().putInt("range", range.ordinal).apply()
        _state.update { it.copy(range = range) }
        viewModelScope.launch { reload() }
    }

    fun setBackgroundSync(on: Boolean) {
        BackgroundSync.setEnabled(getApplication(), on)
        _state.update { it.copy(backgroundSync = on) }
    }

    private fun say(msg: String?, error: Boolean = false) = _state.update { it.copy(message = msg, error = error) }

    /** Runs [block] as the one BLE job in progress, reporting errors. */
    private fun job(block: suspend () -> String) {
        if (_state.value.busy) return
        _state.update { it.copy(busy = true) }
        viewModelScope.launch {
            try {
                say(block())
            } catch (e: Exception) {
                say(e.message ?: e.toString(), error = true)
            } finally {
                _state.update { it.copy(busy = false) }
            }
        }
    }

    /** Finds every logger that is advertising and syncs each one. */
    fun syncNow(forceDays: Int = 0) = job {
        val app = getApplication<Application>()
        say("Looking for loggers… press the button on the logger if it's asleep.")
        val devices = Sync.findAll(app, SCAN_MS)
        if (devices.isEmpty()) throw GattException("No logger found. Press its button and try again.")
        val results = devices.map { d -> Sync.syncLogger(app, d, forceDays) { say(it) } }
        results.firstOrNull()?.let { select(it.id) }
        results.joinToString("\n") { "${it.name}: read ${it.fetched} readings and set the clock." }
    }

    /** Connects to the selected logger only, once it advertises. */
    private suspend fun findSelected(): Pair<LoggerInfo, android.bluetooth.BluetoothDevice> {
        val logger = _state.value.selected ?: throw GattException("No logger selected")
        say("Looking for ${logger.name}… press its button if it's asleep.")
        val device = Sync.find(getApplication(), logger.address, FIND_MS)
            ?: throw GattException("${logger.name} not found. Press its button and try again.")
        return logger to device
    }

    fun rename(name: String) = job {
        val (logger, device) = findSelected()
        say("Renaming…")
        val newName = Sync.rename(getApplication(), device, logger.id, name)
        "Renamed to “$newName”."
    }

    fun clearLogger() = job {
        val (_, device) = findSelected()
        say("Clearing…")
        Sync.clear(getApplication(), device)
        "Logger cleared. The readings already on this phone are kept."
    }

    fun exportCsv(uri: Uri) = job {
        val logger = _state.value.selected ?: throw GattException("No logger selected")
        val app = getApplication<Application>()
        val rows = withContext(Dispatchers.IO) {
            val rows = store.allReadings(logger.id)
            app.contentResolver.openOutputStream(uri)?.bufferedWriter()?.use { w ->
                w.write("ts,time_utc,temp_c,humidity_pct\n")
                for (r in rows) {
                    val iso = if (r.ts == 0L) "" else Instant.ofEpochSecond(r.ts).toString()
                    w.write("${r.ts},$iso,${fmt(r.tempC)},${fmt(r.humidity)}\n")
                }
            } ?: throw GattException("Couldn't open the file")
            rows.size
        }
        "Exported $rows readings."
    }

    // Always a '.' decimal point, whatever the phone's language.
    private fun fmt(v: Double?) = v?.let { String.format(Locale.ROOT, "%.2f", it) } ?: ""

    companion object {
        const val SCAN_MS = 6_000L
        const val FIND_MS = 30_000L
    }
}
