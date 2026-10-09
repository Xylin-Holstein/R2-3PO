package com.robobot3000.r2remote

import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URLEncoder
import java.net.URL

class RemoteApi(private val baseUrl: String, private val token: String) {
    private fun request(method: String, path: String, body: String? = null): JSONObject {
        val normalized = baseUrl.trim().trimEnd('/')
        require(normalized.startsWith("http://") || normalized.startsWith("https://")) {
            "Server address must start with http:// or https://"
        }
        val conn = (URL(normalized + path).openConnection() as HttpURLConnection)
        try {
            conn.requestMethod = method
            conn.connectTimeout = 8000
            conn.readTimeout = 120000
            conn.setRequestProperty("Authorization", "Bearer $token")
            conn.setRequestProperty("Accept", "application/json")
            if (body != null) {
                conn.doOutput = true
                conn.setRequestProperty("Content-Type", "application/json; charset=utf-8")
                conn.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val response = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() } ?: "{}"
            if (code !in 200..299) {
                val message = try { JSONObject(response).optString("error", "HTTP $code") }
                               catch (_: Exception) { "HTTP $code" }
                throw IllegalStateException(message)
            }
            return JSONObject(response)
        } finally {
            conn.disconnect()
        }
    }

    fun status() = request("GET", "/api/status")
    fun chat(message: String) = request("POST", "/api/chat",
        JSONObject().put("message", message).toString()).optString("reply")
    fun diary(limit: Int = 30) = request("GET", "/api/diary?limit=$limit").optString("content")
    fun lifeLog(limit: Int = 50) = request("GET", "/api/life-log?limit=$limit").optString("content")
    fun memories(query: String) = request("GET", "/api/memories?query=" +
        URLEncoder.encode(query, "UTF-8")).optString("content")
}
