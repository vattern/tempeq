package io.github.vattern.thlogger

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

class MainActivity : ComponentActivity() {
    private val vm: MainViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val dark = isSystemInDarkTheme()
            val ctx = LocalContext.current
            MaterialTheme(colorScheme = if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)) {
                MainScreen(vm)
            }
        }
    }
}

private val TEMP_COLOR = Color(0xFFE53935)
private val HUM_COLOR = Color(0xFF1E88E5)
private val timeFmt = DateTimeFormatter.ofPattern("d MMM HH:mm").withZone(ZoneId.systemDefault())

private fun ago(epochMs: Long): String {
    val min = (System.currentTimeMillis() - epochMs) / 60_000
    return when {
        min < 1 -> "just now"
        min < 60 -> "$min min ago"
        min < 48 * 60 -> "${min / 60} h ago"
        else -> "${min / 1440} days ago"
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun MainScreen(vm: MainViewModel) {
    val s by vm.state.collectAsStateWithLifecycle()
    val ctx = LocalContext.current
    var permitted by remember { mutableStateOf(BackgroundSync.hasPermissions(ctx)) }
    val permLauncher = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        permitted = BackgroundSync.hasPermissions(ctx)
        if (permitted) BackgroundSync.restart(ctx)
    }
    val enableBt = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {}
    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/csv")) { uri ->
        uri?.let(vm::exportCsv)
    }
    var menu by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf(false) }
    var clearing by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) { if (permitted) BackgroundSync.restart(ctx) }

    Scaffold(topBar = {
        TopAppBar(
            title = { Text(s.selected?.name ?: "TH-Logger") },
            actions = {
                TextButton(onClick = { menu = true }, enabled = s.selected != null && !s.busy) { Text("More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("Rename logger…") }, onClick = { menu = false; renaming = true })
                    DropdownMenuItem(text = { Text("Re-download last 7 days") }, onClick = { menu = false; vm.syncNow(7) })
                    DropdownMenuItem(text = { Text("Re-download everything") }, onClick = { menu = false; vm.syncNow(Int.MAX_VALUE) })
                    DropdownMenuItem(text = { Text("Export CSV…") }, onClick = {
                        menu = false
                        exportLauncher.launch("${s.selected?.name ?: "th-logger"}.csv")
                    })
                    DropdownMenuItem(text = { Text("Clear logger…") }, onClick = { menu = false; clearing = true })
                }
            },
        )
    }) { pad ->
        Column(
            Modifier.padding(pad).fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            if (!permitted) {
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("The app needs Bluetooth permission to find and read your loggers.")
                        Button(onClick = {
                            permLauncher.launch(arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT))
                        }) { Text("Allow Bluetooth") }
                    }
                }
            } else if (Sync.adapter(ctx)?.isEnabled != true) {
                Card(Modifier.fillMaxWidth()) {
                    Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("Bluetooth is off.")
                        Button(onClick = { enableBt.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }) {
                            Text("Turn on Bluetooth")
                        }
                    }
                }
            }

            if (s.loggers.size > 1) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    s.loggers.forEach { l ->
                        FilterChip(selected = l.id == s.selectedId, onClick = { vm.select(l.id) }, label = { Text(l.name) })
                    }
                }
            }

            CurrentCard(s)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Range.entries.forEach { r ->
                    FilterChip(selected = r == s.range, onClick = { vm.setRange(r) }, label = { Text(r.label) })
                }
            }
            if (s.selected != null) {
                Text("Temperature", style = MaterialTheme.typography.titleSmall)
                TimeChart(s.readings, { it.tempC }, TEMP_COLOR, "°")
                Text("Humidity", style = MaterialTheme.typography.titleSmall)
                TimeChart(s.readings, { it.humidity }, HUM_COLOR, "%")
                MinMax(s.readings)
            }

            Button(onClick = { vm.syncNow() }, enabled = permitted && !s.busy, modifier = Modifier.fillMaxWidth()) {
                Text("Sync now")
            }
            if (s.busy) LinearProgressIndicator(Modifier.fillMaxWidth())
            s.message?.let {
                Text(it, color = if (s.error) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurface)
            }

            Card(Modifier.fillMaxWidth()) {
                Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text("Sync in the background", style = MaterialTheme.typography.titleSmall)
                        Text(
                            "Press a logger's button and the phone reads it, even with this app closed.",
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Spacer(Modifier.width(8.dp))
                    Switch(checked = s.backgroundSync, enabled = permitted, onCheckedChange = vm::setBackgroundSync)
                }
            }
        }
    }

    if (renaming) RenameDialog(s.selected?.name.orEmpty(), onDismiss = { renaming = false }) { renaming = false; vm.rename(it) }
    if (clearing) AlertDialog(
        onDismissRequest = { clearing = false },
        title = { Text("Clear ${s.selected?.name}?") },
        text = { Text("Deletes every reading stored on the logger. This can't be undone. The copy on this phone is kept.") },
        confirmButton = { TextButton(onClick = { clearing = false; vm.clearLogger() }) { Text("Clear") } },
        dismissButton = { TextButton(onClick = { clearing = false }) { Text("Cancel") } },
    )
}

@Composable
private fun CurrentCard(s: UiState) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp)) {
            val r = s.latest
            if (s.selected == null || r == null) {
                Text("No readings yet. Press the button on the logger, then tap Sync now.")
                return@Card
            }
            Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                Text(r.tempC?.let { "%.1f °C".format(it) } ?: "– °C", style = MaterialTheme.typography.displaySmall, color = TEMP_COLOR)
                Text(r.humidity?.let { "%.0f %%".format(it) } ?: "– %", style = MaterialTheme.typography.displaySmall, color = HUM_COLOR)
            }
            Text("Reading from ${timeFmt.format(Instant.ofEpochSecond(r.ts))}", style = MaterialTheme.typography.bodySmall)
            s.selected?.let { l ->
                Text(
                    "Last synced ${ago(l.lastSync)} · ${l.count} readings on this phone",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }
}

@Composable
private fun MinMax(readings: List<Reading>) {
    val t = readings.mapNotNull { it.tempC }
    val h = readings.mapNotNull { it.humidity }
    if (t.isEmpty() && h.isEmpty()) return
    Text(
        buildString {
            if (t.isNotEmpty()) append("Temperature %.1f – %.1f °C".format(t.min(), t.max()))
            if (t.isNotEmpty() && h.isNotEmpty()) append("\n")
            if (h.isNotEmpty()) append("Humidity %.0f – %.0f %%".format(h.min(), h.max()))
        },
        style = MaterialTheme.typography.bodyMedium,
    )
}

@Composable
private fun RenameDialog(current: String, onDismiss: () -> Unit, onRename: (String) -> Unit) {
    var name by remember { mutableStateOf(current) }
    val trimmed = name.trim()
    val valid = trimmed.length <= Th.NAME_MAX_LEN && trimmed.all { it.code in 0x20..0x7e }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Rename logger") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Up to ${Th.NAME_MAX_LEN} plain letters, digits or symbols. Leave empty for the default. Press the logger's button first.")
                OutlinedTextField(value = name, onValueChange = { name = it }, singleLine = true, isError = !valid)
            }
        },
        confirmButton = { TextButton(onClick = { onRename(trimmed) }, enabled = valid) { Text("Rename") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}
