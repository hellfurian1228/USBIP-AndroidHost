package com.mizukos.usbip

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Build
import android.util.Log
import android.widget.Toast
import androidx.core.content.FileProvider
import java.io.File
import java.io.FileOutputStream
import java.io.FileWriter
import java.io.PrintWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

object ErrorLogger {
    private val logBuffer = ConcurrentLinkedQueue<String>()
    private const val MAX_MEMORY_LOGS = 500
    private var logFile: File? = null
    private var isInitialized = false

    var onLogCallback: ((String) -> Unit)? = null

    fun init(context: Context) {
        if (isInitialized) return
        val appContext = context.applicationContext
        try {
            val logsDir = File(appContext.filesDir, "logs")
            if (!logsDir.exists()) logsDir.mkdirs()

            val mainLog = File(logsDir, "usbip_debug.log")
            // Rotate log file if it exceeds 3MB to keep disk usage reasonable
            if (mainLog.exists() && mainLog.length() > 3 * 1024 * 1024) {
                val backupLog = File(logsDir, "usbip_debug_old.log")
                if (backupLog.exists()) backupLog.delete()
                mainLog.renameTo(backupLog)
            }
            logFile = File(logsDir, "usbip_debug.log")

            setupUncaughtExceptionHandler(appContext)
            isInitialized = true

            info("ErrorLogger initialized. Device: ${Build.MANUFACTURER} ${Build.MODEL} (Android ${Build.VERSION.RELEASE}, API ${Build.VERSION.SDK_INT})")
        } catch (e: Exception) {
            Log.e("ErrorLogger", "Failed to initialize ErrorLogger file writer", e)
        }
    }

    private fun setupUncaughtExceptionHandler(context: Context) {
        val defaultHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                val crashReport = StringBuilder()
                crashReport.append("\n========================================\n")
                crashReport.append("FATAL UNCAUGHT EXCEPTION (FORCE CLOSE)\n")
                crashReport.append("Timestamp: ${SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.getDefault()).format(Date())}\n")
                crashReport.append("Thread: ${thread.name} (ID: ${thread.id})\n")
                crashReport.append("Device: ${Build.MANUFACTURER} ${Build.MODEL} (API ${Build.VERSION.SDK_INT})\n")
                crashReport.append("Exception: ${throwable.javaClass.name}: ${throwable.message}\n")
                crashReport.append("Stack trace:\n${Log.getStackTraceString(throwable)}\n")
                crashReport.append("========================================\n")

                val msg = crashReport.toString()
                writeToDisk(msg)
                logBuffer.add(msg)

                // Compress log immediately on force close
                createCompressedLogZip(context, isCrash = true)
            } catch (e: Exception) {
                Log.e("ErrorLogger", "Error handling uncaught exception", e)
            } finally {
                defaultHandler?.uncaughtException(thread, throwable)
            }
        }
    }

    fun log(message: String, throwable: Throwable? = null) {
        error(message, throwable)
    }

    fun info(message: String) {
        appendLog("INFO", message, null)
        Log.i("USBIP_Log", message)
    }

    fun warn(message: String, throwable: Throwable? = null) {
        appendLog("WARN", message, throwable)
        Log.w("USBIP_Log", message, throwable)
    }

    fun error(message: String, throwable: Throwable? = null) {
        appendLog("ERROR", message, throwable)
        Log.e("USBIP_Error", message, throwable)
    }

    private fun appendLog(level: String, message: String, throwable: Throwable?) {
        val timestamp = SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault()).format(Date())
        val stack = throwable?.let { "\n${Log.getStackTraceString(it)}" } ?: ""
        val entry = "[$timestamp] [$level] $message$stack"

        logBuffer.add(entry)
        while (logBuffer.size > MAX_MEMORY_LOGS) {
            logBuffer.poll()
        }

        writeToDisk(entry)
        onLogCallback?.invoke(message)
    }

    @Synchronized
    private fun writeToDisk(entry: String) {
        val target = logFile ?: return
        try {
            FileWriter(target, true).use { writer ->
                PrintWriter(writer).println(entry)
            }
        } catch (_: Exception) { }
    }

    fun getLogs(): String {
        return logBuffer.joinToString("\n\n")
    }

    fun generateSystemReport(context: Context): String {
        val sb = StringBuilder()
        sb.append("--- USB/IP Host System & Debug Report ---\n")
        sb.append("Timestamp: ${Date()}\n")
        sb.append("Device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.PRODUCT})\n")
        sb.append("Android Version: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})\n")
        sb.append("Hardware: ${Build.HARDWARE} / ${Build.BOARD}\n")
        sb.append("ABI Architectures: ${Build.SUPPORTED_ABIS.joinToString(", ")}\n\n")

        try {
            val netInterfaces = getAvailableIpAddresses()
            sb.append("--- Active Network Interfaces (${netInterfaces.size}) ---\n")
            netInterfaces.forEach { iface ->
                sb.append(" - [${iface.interfaceName}] ${iface.displayName}: ${iface.address}\n")
            }
            sb.append("Selected Preferred IP: ${NetworkPreferences.getSelectedIp(context) ?: "Automatic"}\n\n")
        } catch (e: Exception) {
            sb.append("Network Inspection Error: ${e.message}\n\n")
        }

        try {
            val usbManager = context.getSystemService(Context.USB_SERVICE) as? UsbManager
            val devices = usbManager?.deviceList?.values ?: emptyList()
            sb.append("--- Connected USB Hardware (${devices.size}) ---\n")
            devices.forEach { dev ->
                val perm = if (usbManager?.hasPermission(dev) == true) "GRANTED" else "DENIED"
                sb.append(" - ${dev.productName ?: dev.deviceName} (VID: 0x${Integer.toHexString(dev.vendorId)}, PID: 0x${Integer.toHexString(dev.productId)}) Permission: $perm | Class: ${dev.deviceClass} | Interfaces: ${dev.interfaceCount}\n")
            }
            sb.append("\n")
        } catch (e: Exception) {
            sb.append("USB Inspection Error: ${e.message}\n\n")
        }

        return sb.toString()
    }

    fun createCompressedLogZip(context: Context, isCrash: Boolean = false): File? {
        val appContext = context.applicationContext
        try {
            val logsDir = File(appContext.filesDir, "logs")
            if (!logsDir.exists()) logsDir.mkdirs()

            val zipFileName = if (isCrash) "usbip_crash_report.zip" else "usbip_debug_logs.zip"
            val zipFile = File(logsDir, zipFileName)
            if (zipFile.exists()) zipFile.delete()

            val logFileToCompress = logFile ?: File(logsDir, "usbip_debug.log")
            val systemReport = generateSystemReport(appContext)

            ZipOutputStream(FileOutputStream(zipFile)).use { zos ->
                // 1. System Report entry
                zos.putNextEntry(ZipEntry("system_report.txt"))
                zos.write(systemReport.toByteArray(Charsets.UTF_8))
                zos.closeEntry()

                // 2. Main Debug Log File entry
                if (logFileToCompress.exists()) {
                    zos.putNextEntry(ZipEntry("usbip_debug.log"))
                    logFileToCompress.inputStream().use { input ->
                        input.copyTo(zos)
                    }
                    zos.closeEntry()
                } else {
                    // Fallback to memory log buffer if file on disk was not found
                    zos.putNextEntry(ZipEntry("usbip_debug.log"))
                    zos.write(getLogs().toByteArray(Charsets.UTF_8))
                    zos.closeEntry()
                }
            }

            return zipFile
        } catch (e: Exception) {
            Log.e("ErrorLogger", "Failed to create compressed log zip", e)
            return null
        }
    }

    fun shareLogArchive(context: Context) {
        val zipFile = createCompressedLogZip(context, isCrash = false)
        if (zipFile == null || !zipFile.exists()) {
            Toast.makeText(context, "Failed to generate log archive", Toast.LENGTH_SHORT).show()
            return
        }

        try {
            val authority = "${context.packageName}.fileprovider"
            val uri = FileProvider.getUriForFile(context, authority, zipFile)

            val shareIntent = Intent(Intent.ACTION_SEND).apply {
                type = "application/zip"
                putExtra(Intent.EXTRA_STREAM, uri)
                putExtra(Intent.EXTRA_SUBJECT, "USB/IP Host Debug Logs (${Build.MODEL})")
                putExtra(Intent.EXTRA_TEXT, "Attached compressed debug logs and system report for USB/IP Host app.")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }

            context.startActivity(Intent.createChooser(shareIntent, "Share USB/IP Debug Logs"))
        } catch (e: Exception) {
            Log.e("ErrorLogger", "Error sharing log archive", e)
            Toast.makeText(context, "Error launching share intent: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }

    fun copyLogsToClipboard(context: Context) {
        val report = StringBuilder()
        report.append(generateSystemReport(context))
        report.append("--- Recent Activity Logs ---\n")

        if (logBuffer.isEmpty()) {
            report.append("No logs captured yet.")
        } else {
            logBuffer.forEach { report.append(it).append("\n\n") }
        }

        val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = ClipData.newPlainText("USBIP Support Logs", report.toString())
        clipboard.setPrimaryClip(clip)

        Toast.makeText(context, "Support logs & system report copied to clipboard", Toast.LENGTH_SHORT).show()
    }
}
