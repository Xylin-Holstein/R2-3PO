package com.robobot3000.r2remote

import android.os.Bundle
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import org.json.JSONObject
import org.json.JSONArray
import java.util.Locale
import java.util.concurrent.Executors

private val io = Executors.newSingleThreadExecutor()

class MainActivity : ComponentActivity(), TextToSpeech.OnInitListener {
    private var tts: TextToSpeech? = null
    private var ttsReady = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        tts = TextToSpeech(this, this)
        val prefs = try {
            val masterKey = MasterKey.Builder(this)
                .setKeyScheme(MasterKey.KeyScheme.AES256_GCM).build()
            EncryptedSharedPreferences.create(
                this, "r2_remote_secure", masterKey,
                EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
                EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
            )
        } catch (_: Exception) {
            getSharedPreferences("r2_remote_fallback", MODE_PRIVATE)
        }
        setContent {
            MaterialTheme {
                Surface(Modifier.fillMaxSize()) {
                    R2RemoteScreen(
                        initialUrl = prefs.getString("url", "") ?: "",
                        initialToken = prefs.getString("token", "") ?: "",
                        saveSettings = { url, token ->
                            prefs.edit().putString("url", url).putString("token", token).apply()
                        },
                        speak = { text ->
                            if (ttsReady) tts?.speak(text, TextToSpeech.QUEUE_FLUSH, null, "r2-reply")
                        }
                    )
                }
            }
        }
    }

    override fun onInit(status: Int) {
        ttsReady = status == TextToSpeech.SUCCESS
        if (ttsReady) tts?.language = Locale.getDefault()
    }

    override fun onDestroy() {
        tts?.stop()
        tts?.shutdown()
        io.shutdownNow()
        super.onDestroy()
    }
}

private enum class Section(val label: String) {
    CHAT("Chat"), DIARY("Diary"), LIFE_LOG("Life Log"), MEMORIES("Memories"), MEDIA("Media & Calls")
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun R2RemoteScreen(
    initialUrl: String,
    initialToken: String,
    saveSettings: (String, String) -> Unit,
    speak: (String) -> Unit
) {
    val context = LocalContext.current
    var section by remember { mutableStateOf(Section.CHAT) }
    var serverUrl by remember { mutableStateOf(initialUrl) }
    var savedSecret by remember { mutableStateOf(initialToken) }
    var token by remember { mutableStateOf(if (initialToken.isNotBlank()) "PEACE" else "") }
    var draft by remember { mutableStateOf("") }
    var memoryQuery by remember { mutableStateOf("") }
    var output by remember { mutableStateOf("Connect to R2 using your private network address and remote token.") }
    var status by remember { mutableStateOf("Not connected") }
    var busy by remember { mutableStateOf(false) }
    var autoSpeak by remember { mutableStateOf(true) }
    var showR2FaceWindow by remember { mutableStateOf(false) }
    var chat by remember { mutableStateOf(listOf<Pair<String, String>>()) }
    var showSettings by remember { mutableStateOf(initialToken.isBlank()) }
    var settingsError by remember { mutableStateOf("") }

    fun runRequest(action: (RemoteApi) -> String, onSuccess: (String) -> Unit = { output = it }) {
        if (serverUrl.isBlank() || savedSecret.isBlank()) {
            status = "Set the server address and save the real token once; PEACE is only its alias"
            showSettings = true
            return
        }
        busy = true
        status = "Connecting…"
        val url = serverUrl
        val secret = savedSecret
        io.execute {
            try {
                val result = action(RemoteApi(url, secret))
                Handler(Looper.getMainLooper()).post {
                    busy = false
                    status = "Connected"
                    onSuccess(result)
                }
            } catch (e: Exception) {
                Handler(Looper.getMainLooper()).post {
                    busy = false
                    status = "Connection failed"
                    output = e.message ?: "Could not reach R2."
                }
            }
        }
    }


    fun uploadUri(uri: Uri, fallbackMime: String) {
        val mime = context.contentResolver.getType(uri) ?: fallbackMime
        val size = try {
            context.contentResolver.openAssetFileDescriptor(uri, "r")?.use { it.length } ?: -1L
        } catch (_: Exception) { -1L }
        if (size <= 0L || size > 50L * 1024L * 1024L) {
            output = "Cannot upload this file: size must be known and between 1 byte and 50 MiB."
            return
        }
        runRequest({ api ->
            val input = context.contentResolver.openInputStream(uri)
                ?: throw IllegalStateException("Could not open selected media.")
            input.use { api.upload(mime, it, size).toString() }
        }) { raw ->
            val result = JSONObject(raw)
            val mediaType = result.optString("media_type", "media")
            val path = result.optString("path", "")
            val vision = result.optString("vision", "")
            val reply = result.optString("reply", "")
            chat = chat + ("You" to "Uploaded $mediaType: $path")
            if (reply.isNotBlank()) {
                chat = chat + ("R2-3PO" to reply)
                if (autoSpeak) speak(reply)
            }
            output = if (reply.isNotBlank()) reply else vision
        }
    }

    val imagePicker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) uploadUri(uri, "image/jpeg")
    }
    val videoPicker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) uploadUri(uri, "video/mp4")
    }
    val audioPicker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        if (uri != null) uploadUri(uri, "audio/mpeg")
    }

    fun restoreChat(raw: String) {
        val arr = JSONArray(raw)
        val restored = mutableListOf<Pair<String, String>>()
        for (i in 0 until arr.length()) {
            val turn = arr.optJSONObject(i) ?: continue
            val user = turn.optString("user")
            val assistant = turn.optString("assistant")
            if (user.isNotBlank()) restored.add("You" to user)
            if (assistant.isNotBlank()) restored.add("R2-3PO" to assistant)
        }
        chat = restored
        output = if (restored.isEmpty()) "No saved conversation turns found yet." else ""
    }

    LaunchedEffect(serverUrl, savedSecret) {
        if (serverUrl.isNotBlank() && savedSecret.isNotBlank()) {
            runRequest({ api -> api.conversation().toString() }) { raw -> restoreChat(raw) }
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("R2-3PO Remote", fontWeight = FontWeight.Bold)
                        Text(status, style = MaterialTheme.typography.labelSmall)
                    }
                },
                actions = {
                    IconButton(onClick = { showSettings = true }) {
                        Icon(Icons.Default.Settings, contentDescription = "Connection settings")
                    }
                    IconButton(enabled = !busy, onClick = {
                        if (section == Section.CHAT) {
                            runRequest({ api -> api.conversation().toString() }) { raw -> restoreChat(raw) }
                        } else {
                            runRequest({ api ->
                                val s = api.status()
                                "R2 status\nModel: ${s.optString("model")}\nCore initialized: ${s.optBoolean("core_initialized")}\nAutonomous thinking: ${s.optBoolean("thinking_active")}\nMemories: ${s.optInt("memory_count")}"
                            })
                        }
                    }) { Icon(Icons.Default.Refresh, contentDescription = "Refresh current view") }
                }
            )
        },
        bottomBar = {
            NavigationBar {
                Section.values().forEach { item ->
                    NavigationBarItem(
                        selected = section == item,
                        onClick = {
                            section = item
                            when (item) {
                                Section.DIARY -> runRequest({ it.diary() })
                                Section.LIFE_LOG -> runRequest({ it.lifeLog() })
                                Section.MEMORIES -> { output = "Search R2's persistent memories below." }
                                Section.CHAT -> runRequest({ api -> api.conversation().toString() }) { raw -> restoreChat(raw) }
                                else -> Unit
                            }
                        },
                        icon = {
                            Icon(when (item) {
                                Section.CHAT -> Icons.Default.Chat
                                Section.DIARY -> Icons.Default.MenuBook
                                Section.LIFE_LOG -> Icons.Default.Timeline
                                Section.MEMORIES -> Icons.Default.Psychology
                                Section.MEDIA -> Icons.Default.VideoCall
                            }, contentDescription = item.label)
                        },
                        label = { Text(item.label) }
                    )
                }
            }
        }
    ) { padding ->
        Column(
            Modifier.fillMaxSize().padding(padding).padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            when (section) {
                Section.CHAT -> {
                    Text("Your existing R2 core", style = MaterialTheme.typography.titleMedium)
                    Text("This app sends turns to R2's current conversation and memory pipeline.")
                    LazyColumn(
                        Modifier.weight(1f).fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        items(chat) { turn ->
                            Card(Modifier.fillMaxWidth()) {
                                Column(Modifier.padding(12.dp)) {
                                    Text(turn.first, fontWeight = FontWeight.Bold)
                                    Spacer(Modifier.height(4.dp))
                                    Text(turn.second)
                                }
                            }
                        }
                        if (chat.isEmpty()) item {
                            Text(output, modifier = Modifier.padding(vertical = 12.dp))
                        }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        OutlinedTextField(
                            value = draft, onValueChange = { draft = it },
                            modifier = Modifier.weight(1f),
                            placeholder = { Text("Message R2…") },
                            enabled = !busy, maxLines = 4
                        )
                        IconButton(enabled = !busy && draft.isNotBlank(), onClick = {
                            val message = draft.trim()
                            draft = ""
                            chat = chat + ("You" to message)
                            runRequest({ it.chat(message) }) { reply ->
                                chat = chat + ("R2-3PO" to reply)
                                output = reply
                                if (autoSpeak) speak(reply)
                            }
                        }) { Icon(Icons.Default.Send, contentDescription = "Send message") }
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Switch(checked = autoSpeak, onCheckedChange = { autoSpeak = it })
                        Text("Speak R2's replies on this phone")
                    }
                }
                Section.DIARY -> {
                    Text("R2's diary", style = MaterialTheme.typography.titleLarge)
                    Text("Private reflections returned by R2's existing diary subsystem.")
                    Button(enabled = !busy, onClick = { runRequest({ it.diary(50) }) }) {
                        Icon(Icons.Default.Refresh, null); Spacer(Modifier.width(6.dp)); Text("Refresh diary")
                    }
                    Text(output, Modifier.weight(1f).verticalScroll(rememberScrollState()))
                }
                Section.LIFE_LOG -> {
                    Text("R2's Life Log", style = MaterialTheme.typography.titleLarge)
                    Text("Chronological events, separate from his reflective diary.")
                    Button(enabled = !busy, onClick = { runRequest({ it.lifeLog(100) }) }) {
                        Icon(Icons.Default.Refresh, null); Spacer(Modifier.width(6.dp)); Text("Refresh Life Log")
                    }
                    Text(output, Modifier.weight(1f).verticalScroll(rememberScrollState()))
                }
                Section.MEMORIES -> {
                    Text("Persistent memories", style = MaterialTheme.typography.titleLarge)
                    Text("Search the memory system R2 already uses in conversation.")
                    OutlinedTextField(
                        value = memoryQuery, onValueChange = { memoryQuery = it },
                        modifier = Modifier.fillMaxWidth(), label = { Text("Search memories") },
                        enabled = !busy
                    )
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(enabled = !busy, onClick = {
                            runRequest({ it.recentMemories(100) })
                        }, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Default.History, null); Spacer(Modifier.width(6.dp)); Text("Recent memories")
                        }
                        Button(enabled = !busy && memoryQuery.isNotBlank(), onClick = {
                            runRequest({ it.memories(memoryQuery.trim()) })
                        }, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Default.Search, null); Spacer(Modifier.width(6.dp)); Text("Search")
                        }
                    }
                    Text(output, Modifier.weight(1f).verticalScroll(rememberScrollState()))
                }
                Section.MEDIA -> {
                    Text("Media & live calls", style = MaterialTheme.typography.titleLarge)
                    Text("Upload a file to R2's PC. Images and videos are passed through his existing Eyes/vision path when available; audio files are stored for future transcription support.")
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Button(enabled = !busy, onClick = { imagePicker.launch("image/*") }, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Default.Image, null); Spacer(Modifier.width(4.dp)); Text("Photo")
                        }
                        Button(enabled = !busy, onClick = { videoPicker.launch("video/*") }, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Default.VideoLibrary, null); Spacer(Modifier.width(4.dp)); Text("Video")
                        }
                        Button(enabled = !busy, onClick = { audioPicker.launch("audio/*") }, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Default.AudioFile, null); Spacer(Modifier.width(4.dp)); Text("Audio")
                        }
                    }
                    Card(Modifier.fillMaxWidth()) {
                        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Icon(Icons.Default.VideoCall, null); Spacer(Modifier.width(8.dp))
                                Column(Modifier.weight(1f)) {
                                    Text("Live video call with R2", fontWeight = FontWeight.SemiBold)
                                    Text("Calling transport is not enabled yet. The face window below reuses the look of R2's existing V-Webcam popup.", style = MaterialTheme.typography.bodySmall)
                                }
                            }
                            Button(onClick = { showR2FaceWindow = true }, modifier = Modifier.fillMaxWidth()) {
                                Icon(Icons.Default.OpenInNew, null); Spacer(Modifier.width(8.dp))
                                Text("Open R2's face window")
                            }
                        }
                    }
                    Text(output, Modifier.weight(1f).verticalScroll(rememberScrollState()))
                    Text("TTS currently speaks R2's completed text replies on this phone; it is not a live call.")
                }
            }
        }
    }

    if (showR2FaceWindow) {
        Dialog(onDismissRequest = { showR2FaceWindow = false }) {
            Card(
                modifier = Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(containerColor = androidx.compose.ui.graphics.Color(0xFF111820))
            ) {
                Column(verticalArrangement = Arrangement.spacedBy(0.dp)) {
                    Row(
                        Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text("◉  R2-3PO  /  V-WEBCAM", color = androidx.compose.ui.graphics.Color(0xFFE8F0F7),
                            fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
                        Text("● WAITING FOR EYES", color = androidx.compose.ui.graphics.Color(0xFFFFBD59),
                            style = MaterialTheme.typography.labelSmall, fontWeight = FontWeight.Bold)
                    }
                    Card(
                        Modifier.fillMaxWidth().padding(horizontal = 10.dp).height(220.dp),
                        colors = CardDefaults.cardColors(containerColor = androidx.compose.ui.graphics.Color(0xFF05080B))
                    ) {
                        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Icon(Icons.Default.RemoveRedEye, contentDescription = "R2's visual window",
                                    tint = androidx.compose.ui.graphics.Color(0xFF42F58D),
                                    modifier = Modifier.size(54.dp))
                                Text("R2-3PO", color = androidx.compose.ui.graphics.Color(0xFF42F58D),
                                    fontWeight = FontWeight.Bold)
                                Text("EYES VIEW", color = androidx.compose.ui.graphics.Color(0xFF8FA3B7),
                                    style = MaterialTheme.typography.labelSmall)
                            }
                        }
                    }
                    Column(Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(5.dp)) {
                        Text("Source: waiting for R2's Eyes stream", color = androidx.compose.ui.graphics.Color(0xFFE8F0F7),
                            fontWeight = FontWeight.SemiBold)
                        Text("The existing V-Webcam popup's dark canvas, green identity accents, live status, and source panel are represented here. The live remote frame transport is not connected yet.",
                            color = androidx.compose.ui.graphics.Color(0xFF8FA3B7),
                            style = MaterialTheme.typography.bodySmall)
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            OutlinedButton(enabled = false, onClick = {}, modifier = Modifier.weight(1f)) { Text("Pause display") }
                            OutlinedButton(enabled = false, onClick = {}, modifier = Modifier.weight(1f)) { Text("Hide focus box") }
                        }
                        Text("Display only • no camera • no control of Eyes", color = androidx.compose.ui.graphics.Color(0xFF8FA3B7),
                            style = MaterialTheme.typography.labelSmall)
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                            TextButton(onClick = { showR2FaceWindow = false }) { Text("Close window") }
                        }
                    }
                }
            }
        }
    }

    if (showSettings) {
        AlertDialog(
            onDismissRequest = { showSettings = false },
            title = { Text("Connect to your R2") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text("Enter the PC's private gateway address. Paste the generated token printed by R2's launcher once. After it is saved, PEACE or a blank token field reuses that saved credential.")
                    OutlinedTextField(value = serverUrl, onValueChange = { serverUrl = it; settingsError = "" },
                        label = { Text("Gateway URL") }, singleLine = true)
                    OutlinedTextField(value = token, onValueChange = { token = it; settingsError = "" },
                        label = { Text("Remote token or alias (PEACE)") }, singleLine = true)
                    if (settingsError.isNotBlank()) {
                        Text(settingsError, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall)
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = {
                    val url = serverUrl.trim()
                    val entered = token.trim()
                    val effectiveSecret = if (entered.isBlank() || entered.equals("PEACE", ignoreCase = true)) savedSecret else entered
                    when {
                        url.isBlank() -> {
                            settingsError = "Enter the gateway URL first (for example, http://100.x.y.z:8765)."
                        }
                        effectiveSecret.isBlank() -> {
                            settingsError = "PEACE is an alias, not the password. Paste the real token printed by R2's launcher once, then tap Save & connect."
                            status = "Token needed"
                        }
                        else -> {
                            settingsError = ""
                            serverUrl = url
                            savedSecret = effectiveSecret
                            token = "PEACE"
                            saveSettings(url, effectiveSecret)
                            showSettings = false
                            runRequest({ api ->
                                val s = api.status()
                                "Connected to ${s.optString("service")}\nModel: ${s.optString("model")}"
                            })
                        }
                    }
                }) { Text("Save & connect") }
            },
            dismissButton = { TextButton(onClick = { showSettings = false }) { Text("Cancel") } }
        )
    }
}
