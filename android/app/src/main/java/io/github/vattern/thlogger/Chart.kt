package io.github.vattern.thlogger

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import kotlin.math.abs
import kotlin.math.max

private const val BUCKETS = 300
private const val GAP_S = 1800
/** Smallest window you can zoom in to. */
private const val MIN_SPAN_S = 3600L
private val AXIS_LEFT = 48.dp
private val AXIS_BOTTOM = 18.dp
private val TOP = 6.dp

/** The time window shown, in unix seconds. */
data class Viewport(val start: Long, val end: Long) {
    val span: Long get() = max(1L, end - start)

    /** Zooms by [factor] (> 1 = in) around time [at], then shifts by [shiftS], staying inside [bounds]. */
    fun transform(factor: Float, at: Long, shiftS: Long, bounds: Viewport): Viewport {
        val newSpan = (span / factor).toLong().coerceIn(minOf(MIN_SPAN_S, bounds.span), bounds.span)
        val frac = (at - start).toDouble() / span
        var s = at - (frac * newSpan).toLong() + shiftS
        s = s.coerceIn(bounds.start, bounds.end - newSpan)
        return Viewport(s, s + newSpan)
    }
}

/** Time span covered by these readings (null if none have a known time). */
fun fullViewport(readings: List<Reading>): Viewport? =
    if (readings.isEmpty()) null else Viewport(readings.first().ts, max(readings.last().ts, readings.first().ts + 1))

/** First index with ts ≥ [t] (readings are in record order, which is time order). */
private fun lowerBound(readings: List<Reading>, t: Long): Int {
    var lo = 0; var hi = readings.size
    while (lo < hi) {
        val mid = (lo + hi) ushr 1
        if (readings[mid].ts < t) lo = mid + 1 else hi = mid
    }
    return lo
}

/** The reading closest in time to [t]. */
fun nearestReading(readings: List<Reading>, t: Long): Reading? {
    if (readings.isEmpty()) return null
    val i = lowerBound(readings, t)
    return listOfNotNull(readings.getOrNull(i - 1), readings.getOrNull(i)).minByOrNull { abs(it.ts - t) }
}

/** Min, mean and max per time bucket, so long ranges stay fast to draw. Null = no data. */
private class Bucket(val lo: Float, val mean: Float, val hi: Float)

private class ChartData(val buckets: List<Bucket?>, val lo: Float, val hi: Float)

private fun chartData(readings: List<Reading>, value: (Reading) -> Double?, view: Viewport): ChartData? {
    val from = lowerBound(readings, view.start)
    val to = lowerBound(readings, view.end + 1)
    val sum = DoubleArray(BUCKETS); val n = IntArray(BUCKETS)
    val lo = FloatArray(BUCKETS) { Float.MAX_VALUE }; val hi = FloatArray(BUCKETS) { -Float.MAX_VALUE }
    for (i in from until to) {
        val r = readings[i]
        val v = value(r) ?: continue
        val b = (((r.ts - view.start).toDouble() / view.span) * (BUCKETS - 1)).toInt().coerceIn(0, BUCKETS - 1)
        sum[b] += v; n[b]++
        lo[b] = minOf(lo[b], v.toFloat()); hi[b] = maxOf(hi[b], v.toFloat())
    }
    val buckets = List(BUCKETS) { if (n[it] == 0) null else Bucket(lo[it], (sum[it] / n[it]).toFloat(), hi[it]) }
    val present = buckets.filterNotNull()
    if (present.isEmpty()) return null
    var min = present.minOf { it.lo }; var max = present.maxOf { it.hi }
    if (max - min < 1f) { val mid = (max + min) / 2; min = mid - 0.5f; max = mid + 0.5f }
    return ChartData(buckets, min, max)
}

private fun Density.plotWidth(width: Float) = width - AXIS_LEFT.toPx()
private fun Density.timeAt(x: Float, width: Float, view: Viewport): Long =
    view.start + ((x - AXIS_LEFT.toPx()) / plotWidth(width) * view.span).toLong()

private val axisDays = DateTimeFormatter.ofPattern("d MMM").withZone(ZoneId.systemDefault())
private val axisHours = DateTimeFormatter.ofPattern("HH:mm").withZone(ZoneId.systemDefault())
private val pointTime = DateTimeFormatter.ofPattern("EEE d MMM HH:mm").withZone(ZoneId.systemDefault())

/**
 * A line chart of one series over [view], with a min–max band where readings are bunched together.
 * Pinch to zoom, drag sideways to pan (both report through [onViewChange]); tap to pick the
 * nearest reading ([onSelect]), tap it again to clear.
 */
@Composable
fun TimeChart(
    readings: List<Reading>,
    value: (Reading) -> Double?,
    color: Color,
    unit: String,
    view: Viewport,
    bounds: Viewport,
    onViewChange: (Viewport) -> Unit,
    selected: Reading?,
    onSelect: (Reading?) -> Unit,
) {
    val measurer = rememberTextMeasurer()
    val labelStyle = TextStyle(fontSize = MaterialTheme.typography.labelSmall.fontSize, color = MaterialTheme.colorScheme.onSurfaceVariant)
    val tipStyle = TextStyle(
        fontSize = MaterialTheme.typography.labelMedium.fontSize, fontWeight = FontWeight.Medium,
        color = MaterialTheme.colorScheme.inverseOnSurface,
    )
    val tipBg = MaterialTheme.colorScheme.inverseSurface
    val grid = MaterialTheme.colorScheme.outlineVariant
    val cursor = MaterialTheme.colorScheme.onSurfaceVariant
    val data = remember(readings, view) { chartData(readings, value, view) }

    // Gesture handlers outlive recompositions; read the latest values through these.
    val currentView by rememberUpdatedState(view)
    val currentBounds by rememberUpdatedState(bounds)
    val currentSelected by rememberUpdatedState(selected)
    val currentReadings by rememberUpdatedState(readings)
    val touchSlop = LocalViewConfiguration.current.touchSlop

    val gestures = Modifier
        .pointerInput(Unit) {
            detectTapGestures { pos ->
                val w = size.width.toFloat()
                val v = currentView
                val sel = currentSelected
                val selX = sel?.let { AXIS_LEFT.toPx() + (it.ts - v.start).toFloat() / v.span * plotWidth(w) }
                if (selX != null && abs(selX - pos.x) < 24.dp.toPx()) onSelect(null)
                else onSelect(nearestReading(currentReadings, timeAt(pos.x, w, v)))
            }
        }
        .pointerInput(Unit) {
            // Two fingers: zoom (and pan). One finger: pan once it moves sideways more than
            // vertically; vertical drags are left alone so the page still scrolls.
            awaitEachGesture {
                awaitFirstDown(requireUnconsumed = false)
                var panning = false
                var dragX = 0f; var dragY = 0f
                do {
                    val event = awaitPointerEvent()
                    val fingers = event.changes.count { it.pressed }
                    val pan = event.calculatePan()
                    val w = size.width.toFloat()
                    val v = currentView
                    if (fingers >= 2) {
                        val zoom = event.calculateZoom()
                        val at = timeAt(event.calculateCentroid().x, w, v)
                        val shift = (-pan.x / plotWidth(w) * v.span).toLong()
                        onViewChange(v.transform(zoom, at, shift, currentBounds))
                        event.changes.forEach { it.consume() }
                    } else if (fingers == 1) {
                        if (!panning) {
                            dragX += pan.x; dragY += pan.y
                            if (abs(dragX) > touchSlop && abs(dragX) > abs(dragY)) panning = true
                            else if (abs(dragY) > touchSlop) break
                        }
                        if (panning) {
                            val shift = (-pan.x / plotWidth(w) * v.span).toLong()
                            onViewChange(v.transform(1f, v.start, shift, currentBounds))
                            event.changes.forEach { it.consume() }
                        }
                    }
                } while (event.changes.any { it.pressed })
            }
        }

    Canvas(Modifier.fillMaxWidth().height(170.dp).then(gestures)) {
        if (data == null) {
            drawText(measurer, "No readings in this window", Offset(AXIS_LEFT.toPx(), size.height / 2), labelStyle)
            return@Canvas
        }
        val left = AXIS_LEFT.toPx(); val bottom = size.height - AXIS_BOTTOM.toPx(); val top = TOP.toPx()
        val w = plotWidth(size.width)
        fun xOfBucket(i: Int) = left + w * i / (BUCKETS - 1)
        fun xOfTime(t: Long) = left + w * (t - view.start).toFloat() / view.span
        fun y(v: Float) = bottom - (bottom - top) * (v - data.lo) / (data.hi - data.lo)

        for (v in listOf(data.lo, (data.lo + data.hi) / 2, data.hi)) {
            drawLine(grid, Offset(left, y(v)), Offset(size.width, y(v)))
            val label = measurer.measure("%.1f%s".format(v, unit.trim()), labelStyle)
            drawText(label, topLeft = Offset(0f, y(v) - label.size.height / 2))
        }
        val fmt = if (view.span > 2 * 86_400) axisDays else axisHours
        val ticks = listOf(view.start, view.start + view.span / 2, view.end)
        ticks.forEachIndexed { i, t ->
            val label = measurer.measure(fmt.format(Instant.ofEpochSecond(t)), labelStyle)
            val x = when (i) {
                0 -> left
                ticks.lastIndex -> size.width - label.size.width
                else -> xOfTime(t) - label.size.width / 2
            }
            drawText(label, topLeft = Offset(x, bottom + 2.dp.toPx()))
        }

        val band = color.copy(alpha = 0.18f)
        val line = Path()
        // Break the line where readings are missing for more than half an hour.
        val bucketS = view.span.toDouble() / (BUCKETS - 1)
        var last = -1
        data.buckets.forEachIndexed { i, b ->
            if (b == null) return@forEachIndexed
            if (b.hi > b.lo) drawLine(band, Offset(xOfBucket(i), y(b.lo)), Offset(xOfBucket(i), y(b.hi)), strokeWidth = w / BUCKETS + 1)
            val gap = last < 0 || (i - last > 1 && (i - last) * bucketS > GAP_S)
            if (gap) line.moveTo(xOfBucket(i), y(b.mean)) else line.lineTo(xOfBucket(i), y(b.mean))
            last = i
        }
        drawPath(line, color, style = Stroke(width = 2.dp.toPx()))

        // The picked reading: a cursor line, a dot on the series and a label with time and value.
        if (selected != null && selected.ts in view.start..view.end) {
            val x = xOfTime(selected.ts)
            drawLine(cursor, Offset(x, top), Offset(x, bottom), strokeWidth = 1.dp.toPx())
            val v = value(selected)
            if (v != null) {
                val cy = y(v.toFloat()).coerceIn(top, bottom)
                drawCircle(color, radius = 4.dp.toPx(), center = Offset(x, cy))
            }
            val text = pointTime.format(Instant.ofEpochSecond(selected.ts)) + "  " +
                (v?.let { "%.1f%s".format(it, unit) } ?: "no reading")
            val tip = measurer.measure(text, tipStyle)
            val pad = 6.dp.toPx()
            val boxW = tip.size.width + 2 * pad
            val boxH = tip.size.height + pad
            // Beside the cursor, on whichever side has room.
            val bx = (if (x + 8.dp.toPx() + boxW <= size.width) x + 8.dp.toPx() else x - 8.dp.toPx() - boxW)
                .coerceIn(left, max(left, size.width - boxW))
            drawRoundRect(tipBg, Offset(bx, top), Size(boxW, boxH), CornerRadius(4.dp.toPx()))
            drawText(tip, topLeft = Offset(bx + pad, top + pad / 2))
        }
    }
}
