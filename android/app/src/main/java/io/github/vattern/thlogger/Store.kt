package io.github.vattern.thlogger

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class LoggerInfo(
    val id: String,
    val name: String,
    val address: String,
    /** The record number to fetch from next time. */
    val next: Long,
    val intervalS: Int,
    val capacity: Long,
    val count: Long,
    val lastSync: Long,
    /** Logger clock minus phone clock at the last sync, in seconds. */
    val clockOffset: Long,
)

data class Reading(val ts: Long, val tempC: Double?, val humidity: Double?)

/** All readings, kept per logger (by its ID) and keyed by record number so re-reads don't duplicate. */
class Store private constructor(context: Context) :
    SQLiteOpenHelper(context, "thlogger.db", null, 1) {

    private val _changes = MutableStateFlow(0L)
    /** Bumped on every write, so the UI can reload. */
    val changes: StateFlow<Long> = _changes

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE logger (id TEXT PRIMARY KEY, name TEXT NOT NULL, address TEXT NOT NULL,
               next INTEGER NOT NULL, interval_s INTEGER NOT NULL, capacity INTEGER NOT NULL,
               last_sync INTEGER NOT NULL, clock_offset INTEGER NOT NULL)""",
        )
        db.execSQL(
            """CREATE TABLE reading (logger TEXT NOT NULL, rec INTEGER NOT NULL, ts INTEGER NOT NULL,
               temp INTEGER, hum INTEGER, PRIMARY KEY (logger, rec)) WITHOUT ROWID""",
        )
        db.execSQL("CREATE INDEX reading_ts ON reading (logger, ts)")
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {}

    private fun changed() {
        _changes.value++
    }

    fun loggers(): List<LoggerInfo> = readableDatabase.rawQuery(
        """SELECT id, name, address, next, interval_s, capacity, last_sync, clock_offset,
           (SELECT COUNT(*) FROM reading WHERE logger = id) FROM logger ORDER BY name""",
        null,
    ).use { c ->
        buildList {
            while (c.moveToNext()) add(
                LoggerInfo(
                    id = c.getString(0), name = c.getString(1), address = c.getString(2),
                    next = c.getLong(3), intervalS = c.getInt(4), capacity = c.getLong(5),
                    lastSync = c.getLong(6), clockOffset = c.getLong(7), count = c.getLong(8),
                ),
            )
        }
    }

    fun logger(id: String): LoggerInfo? = loggers().firstOrNull { it.id == id }

    fun loggerByAddress(address: String): LoggerInfo? = loggers().firstOrNull { it.address == address }

    /** Readings with timestamp ≥ [sinceTs], oldest first. The default leaves out unknown-time (0) ones. */
    fun readings(id: String, sinceTs: Long = 1): List<Reading> = readableDatabase.rawQuery(
        "SELECT ts, temp, hum FROM reading WHERE logger = ? AND ts >= ? ORDER BY rec",
        arrayOf(id, sinceTs.toString()),
    ).use { c ->
        buildList(c.count) {
            while (c.moveToNext()) add(
                Reading(
                    c.getLong(0),
                    if (c.isNull(1)) null else c.getInt(1) / 100.0,
                    if (c.isNull(2)) null else c.getInt(2) / 100.0,
                ),
            )
        }
    }

    /** Every reading including unknown-time ones (timestamp 0), for CSV export. */
    fun allReadings(id: String): List<Reading> = readings(id, 0)

    fun upsertLogger(info: LoggerInfo) {
        writableDatabase.insertWithOnConflict(
            "logger", null,
            ContentValues().apply {
                put("id", info.id); put("name", info.name); put("address", info.address)
                put("next", info.next); put("interval_s", info.intervalS); put("capacity", info.capacity)
                put("last_sync", info.lastSync); put("clock_offset", info.clockOffset)
            },
            SQLiteDatabase.CONFLICT_REPLACE,
        )
        changed()
    }

    fun rename(id: String, name: String) {
        writableDatabase.update("logger", ContentValues().apply { put("name", name) }, "id = ?", arrayOf(id))
        changed()
    }

    fun insertRecords(id: String, startRec: Long, records: List<Record>) {
        val db = writableDatabase
        db.beginTransaction()
        try {
            val st = db.compileStatement("INSERT OR REPLACE INTO reading VALUES (?, ?, ?, ?, ?)")
            records.forEachIndexed { i, r ->
                st.clearBindings()
                st.bindString(1, id)
                st.bindLong(2, startRec + i)
                st.bindLong(3, r.ts)
                r.tempC100?.let { st.bindLong(4, it.toLong()) } ?: st.bindNull(4)
                r.hum100?.let { st.bindLong(5, it.toLong()) } ?: st.bindNull(5)
                st.executeInsert()
            }
            db.setTransactionSuccessful()
        } finally {
            db.endTransaction()
        }
        changed()
    }

    /** Drops this phone's copy of a logger's readings (used when its record numbers restart). */
    fun deleteReadings(id: String) {
        writableDatabase.delete("reading", "logger = ?", arrayOf(id))
        changed()
    }

    companion object {
        @Volatile private var instance: Store? = null
        fun get(context: Context): Store =
            instance ?: synchronized(this) { instance ?: Store(context.applicationContext).also { instance = it } }
    }
}
