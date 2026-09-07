package com.mizukos.usbip

import android.util.Log
import kotlinx.coroutines.*
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicBoolean

/**
 * A lightweight, read-only HTTP server providing USB/IP device telemetry on port 3241.
 */
class TelemetryServer(
    private val port: Int = 3241,
    private val getJsonProvider: () -> String,
) {
    private val tag = "TelemetryServer"
    private var serverSocket: ServerSocket? = null
    private val isRunning = AtomicBoolean(false)
    private var serverJob: Job? = null

    fun start(scope: CoroutineScope) {
        if (isRunning.get()) return
        isRunning.set(true)
        serverJob = scope.launch(Dispatchers.IO) {
            try {
                serverSocket = ServerSocket(port)
                Log.i(tag, "Telemetry HTTP server started on port $port")
                while (isRunning.get()) {
                    try {
                        val clientSocket = serverSocket?.accept() ?: break
                        launch(Dispatchers.IO) {
                            handleClient(clientSocket)
                        }
                    } catch (e: Exception) {
                        if (!isRunning.get()) break
                        Log.e(tag, "Error accepting client connection: ${e.message}")
                    }
                }
            } catch (e: Exception) {
                Log.e(tag, "Failed to start telemetry server on port $port: ${e.message}")
            } finally {
                stop()
            }
        }
    }

    private fun handleClient(socket: Socket) {
        try {
            socket.soTimeout = 5000 // 5 seconds timeout
            val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
            val requestLine = reader.readLine() ?: return
            
            // Read headers (drain reader up to empty line)
            var line: String?
            while (reader.readLine().also { line = it } != null) {
                if (line!!.isEmpty()) break
            }

            val parts = requestLine.split(" ")
            if (parts.size < 2) {
                sendResponse(socket, 400, "Bad Request")
                return
            }

            val method = parts[0]
            val path = parts[1]

            if (method != "GET") {
                sendResponse(socket, 405, "Method Not Allowed")
                return
            }

            when (path) {
                "/telemetry" -> {
                    val json = getJsonProvider()
                    sendJsonResponse(socket, json)
                }
                "/health" -> {
                    sendJsonResponse(socket, "{\"status\":\"healthy\"}")
                }
                else -> {
                    sendResponse(socket, 404, "Not Found")
                }
            }
        } catch (e: Exception) {
            Log.e(tag, "Error handling telemetry request: ${e.message}")
            try {
                sendResponse(socket, 500, "Internal Server Error")
            } catch (_: Exception) {}
        } finally {
            try {
                socket.close()
            } catch (_: Exception) {}
        }
    }

    private fun sendJsonResponse(socket: Socket, json: String) {
        val writer = PrintWriter(socket.getOutputStream(), true)
        writer.print("HTTP/1.1 200 OK\r\n")
        writer.print("Content-Type: application/json; charset=UTF-8\r\n")
        writer.print("Content-Length: ${json.toByteArray(Charsets.UTF_8).size}\r\n")
        writer.print("Connection: close\r\n")
        writer.print("\r\n")
        writer.print(json)
        writer.flush()
    }

    private fun sendResponse(socket: Socket, statusCode: Int, message: String) {
        val writer = PrintWriter(socket.getOutputStream(), true)
        writer.print("HTTP/1.1 $statusCode ${getStatusText(statusCode)}\r\n")
        writer.print("Content-Type: text/plain; charset=UTF-8\r\n")
        writer.print("Content-Length: ${message.length}\r\n")
        writer.print("Connection: close\r\n")
        writer.print("\r\n")
        writer.print(message)
        writer.flush()
    }

    private fun getStatusText(code: Int): String = when (code) {
        200 -> "OK"
        400 -> "Bad Request"
        404 -> "Not Found"
        405 -> "Method Not Allowed"
        else -> "Internal Server Error"
    }

    fun stop() {
        if (!isRunning.getAndSet(false)) return
        try {
            serverSocket?.close()
        } catch (_: Exception) {}
        serverSocket = null
        serverJob?.cancel()
        Log.i(tag, "Telemetry HTTP server stopped.")
    }
}
