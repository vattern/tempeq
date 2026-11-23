package io.github.vattern.thlogger

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.max

private const val BUCKETS = 300
private const val GAP_S = 1800

/** Min, mean and max per time bucket, so long ranges stay fast to draw. Null = no data. */
private data class Bucket(val lo: Float, val mean: Float, val hi: Float)

private fun bucketize(ts: LongArray, values: List<Double?>, t0: Long, t1: Long): List<Bucket?> {
    val span = max(1L, t1 - t0)
    val sum = DoubleArray(BUCKETS); val n = IntArray(BUCKETS)
    val lo = FloatArray(BUCKETS) { Float.MAX_VALUE }; val hi = FloatArray(BUCKETS) { -Float.MAX_VALUE }
    for (i in ts.indices) {
        val v = values[i] ?: continue
        val b = (((ts[i] - t0).toDouble() / span) * (BUCKETS - 1)).toInt().coerceIn(0, BUCKETS - 1)
        sum[b] += v; n[b]++
        lo[b] = minOf(lo[b], v.toFloat()); hi[b] = maxOf(hi[b], v.toFloat())
    }
    return List(BUCKETS) { if (n[it] == 0) null else Bucket(lo[it], (sum[it] / n[it]).toFloat(), hi[it]) }
}

/** A line chart of one series over time, with a min–max band where readings are bunched together. */
@Composable
fun TimeChart(readings: List<Reading>, value: (Reading) -> Double?, color: Color, unit: String) {
    val measurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontSize = MaterialTheme.typography.labelSmall.fontSize, color = MaterialTheme.colorScheme.onSurfaceVariant)
    val grid = MaterialTheme.colorScheme.outlineVariant
    val data = remember(readings) {
        val ts = LongArray(readings.size) { readings[it].ts }
        val vals = readings.map(value)
        if (ts.isEmpty() || vals.all { it == null }) null
        else {
            val t0 = ts.first(); val t1 = ts.last()
            val buckets = bucketize(ts, vals, t0, t1)
            val present = buckets.filterNotNull()
            var lo = present.minOf { it.lo }; var hi = present.maxOf { it.hi }
            if (hi - lo < 1f) { val mid = (hi + lo) / 2; lo = mid - 0.5f; hi = mid + 0.5f }
            ChartData(t0, t1, buckets, lo, hi)
        }
    }
    Canvas(Modifier.fillMaxWidth().height(160.dp)) {
        if (data == null) {
            drawText(measurer, "No readings in this range", Offset(0f, size.height / 2), labelStyle)
            return@Canvas
        }
        val left = 48.dp.toPx(); val bottom = size.height - 18.dp.toPx(); val top = 6.dp.toPx()
        val w = size.width - left
        fun x(i: Int) = left + w * i / (BUCKETS - 1)
        fun y(v: Float) = bottom - (bottom - top) * (v - data.lo) / (data.hi - data.lo)

        for (v in listOf(data.lo, (data.lo + data.hi) / 2, data.hi)) {
            drawLine(grid, Offset(left, y(v)), Offset(size.width, y(v)))
            val label = measurer.measure("%.1f%s".format(v, unit), labelStyle)
            drawText(label, topLeft = Offset(0f, y(v) - label.size.height / 2))
        }
        val fmt = DateTimeFormatter.ofPattern(if (data.t1 - data.t0 > 2 * 86_400) "d MMM" else "HH:mm")
            .withZone(ZoneId.systemDefault())
        val start = measurer.measure(fmt.format(Instant.ofEpochSecond(data.t0)), labelStyle)
        val end = measurer.measure(fmt.format(Instant.ofEpochSecond(data.t1)), labelStyle)
        drawText(start, topLeft = Offset(left, bottom + 2.dp.toPx()))
        drawText(end, topLeft = Offset(size.width - end.size.width, bottom + 2.dp.toPx()))

        val band = color.copy(alpha = 0.18f)
        val line = Path()
        // Break the line where readings are missing for more than half an hour.
        val bucketS = (data.t1 - data.t0).toDouble() / (BUCKETS - 1)
        var last = -1
        data.buckets.forEachIndexed { i, b ->
            if (b == null) return@forEachIndexed
            if (b.hi > b.lo) drawLine(band, Offset(x(i), y(b.lo)), Offset(x(i), y(b.hi)), strokeWidth = w / BUCKETS + 1)
            val gap = last < 0 || (i - last > 1 && (i - last) * bucketS > GAP_S)
            if (gap) line.moveTo(x(i), y(b.mean)) else line.lineTo(x(i), y(b.mean))
            last = i
        }
        drawPath(line, color, style = Stroke(width = 2.dp.toPx()))
    }
}

private class ChartData(val t0: Long, val t1: Long, val buckets: List<Bucket?>, val lo: Float, val hi: Float)
