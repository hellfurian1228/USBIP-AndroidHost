package com.mizukos.usbip

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.util.Log
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.net.wifi.WifiManager
import android.widget.Toast
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat

import android.os.Binder
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.LinkedList
import java.util.Queue
import java.util.concurrent.ConcurrentHashMap
import kotlin.time.Duration.Companion.seconds

class UsbServerService : Service() {

    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val nativeServerMutex = Mutex()
    private var busIdCounter = 1
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    private val binder = LocalBinder()

    inner class LocalBinder : Binder() {
        fun getService(): UsbServerService = this@UsbServerService
    }

    override fun onBind(intent: Intent?): IBinder = binder

    private lateinit var usbManager: UsbManager
    private var isNativeServerStarted = false
    private lateinit var usbipNsdManager: UsbipNsdManager

    data class DeviceHandle(
        val device: UsbDevice,
        val connection: UsbDeviceConnection,
        val busId: String,
        val profile: SpecialDeviceProfile
    )

    enum class SpecialDeviceProfile {
        LOGITECH_G29,
        ETHERNET,
        STEAM_CONTROLLER, // Add this profile
        GENERIC
    }

    data class DeviceInfo(
        val deviceId: Int,
        val busId: String,
        val productName: String,
        val vendorId: Int,
        val productId: Int,
        val isConnected: Boolean
    )

    private val openedDevices = ConcurrentHashMap<String, DeviceHandle>()
    private val deviceJobs = ConcurrentHashMap<String, Job>()
    private val pendingConnections = ConcurrentHashMap<String, Int>() // busId to deviceId
    
    // Explicit Session Guard: Prevents multiple bindings for the same physical device ID
    private val activeDeviceIdTracker = ConcurrentHashMap<Int, String>() // deviceId to busId
    private val deviceNameToBusId = ConcurrentHashMap<String, String>() // deviceName to busId
    
    // Tracks devices user explicitly wanted to export (survives G29 mode switch resets)
    private val authorizedBusIds = mutableSetOf<String>()
    
    // UI Synchronization
    private val _deviceList = MutableStateFlow<Map<String, DeviceInfo>>(emptyMap())
    val deviceList: StateFlow<Map<String, DeviceInfo>> = _deviceList.asStateFlow()

    // Permission Queuing
    private val permissionQueue: Queue<UsbDevice> = LinkedList()
    private var isRequestingPermission = false

    // JNI accessors (Now querying the map)
    @Suppress("unused")
    fun getVidForBusId(busId: String): Int = openedDevices[busId]?.device?.vendorId ?: 0
    @Suppress("unused")
    fun getPidForBusId(busId: String): Int = openedDevices[busId]?.device?.productId ?: 0
    @Suppress("unused")
    fun getInterfaceCountForBusId(busId: String): Int = openedDevices[busId]?.device?.interfaceCount ?: 0
    
    @Suppress("unused")
    fun getFdForBusId(busId: String): Int = openedDevices[busId]?.connection?.fileDescriptor ?: -1

    @Suppress("unused")
    fun getSpeedForBusId(busId: String): Int {
        val device = openedDevices[busId]?.device ?: return 3
        if (Build.VERSION.SDK_INT >= 31) {
            try {
                val s = device.javaClass.getMethod("getSpeed").invoke(device) as Int
                return when (s) {
                    1 -> 1 // LOW
                    2 -> 2 // FULL
                    3 -> 3 // HIGH
                    4 -> 5 // SUPER
                    5 -> 5 // SUPER_PLUS
                    else -> 3
                }
            } catch (_: Exception) { }
        }
        return 3
    }

    fun connectDeviceManually(device: UsbDevice) {
        val busId = getBusId(device)
        // Manual override: clear any stuck pending state to allow a fresh connection attempt
        pendingConnections.remove(busId)
        authorizedBusIds.add(busId)
        handleIncomingDevice(device)
    }

    fun disconnectDeviceManually(deviceId: Int) {
        serviceScope.launch {
            val busId = activeDeviceIdTracker[deviceId]
            if (busId != null) {
                authorizedBusIds.remove(busId)
                performComprehensiveCleanup(busId, deviceId)
            } else {
                // Fallback for UI sync if tracker is missing
                openedDevices.entries.find { it.value.device.deviceId == deviceId }?.let {
                    performComprehensiveCleanup(it.key, deviceId)
                }
            }
        }
    }

    private fun performComprehensiveCleanup(busId: String, deviceId: Int) {
        Log.i("UsbServerService", "Unified Cleanup Triggered: Bus $busId, Device $deviceId")
        
        // 0. Cancel any pending jobs or async handshakes
        deviceJobs.remove(busId)?.cancel()
        pendingConnections.remove(busId)
        
        // 1. Force native TCP teardown and invalidation (This calls shutdown() on sockets)
        invalidateDeviceFd(busId)
        
        // 2. Close active connection and release FD
        val handle = openedDevices.remove(busId)
        try {
            handle?.connection?.close()
        } catch (_: Exception) {
            // Error already logged by higher level or connection is already dead
        }
        
        // 3. Purge session trackers
        activeDeviceIdTracker.remove(deviceId)
        
        // 5. Reactive UI Sync
        updateUiState()
    }

    /**
     * Build a raw USB/IP OP_REP_DEVLIST body (number of devices + devices + interfaces)
     * Queries UsbManager directly to ensure no ghost entries or stale metadata.
     */
    @Suppress("unused")
    fun getExportedDevicesPayload(): ByteArray {
        val currentHardware = usbManager.deviceList.values
        // Real-time synchronization: filter hardware by active/authorized handles
        val exported = currentHardware.filter { dev ->
            openedDevices.values.any { it.device.deviceName == dev.deviceName }
        }

        if (exported.isEmpty()) {
            return ByteBuffer.allocate(4).apply { putInt(0) }.array()
        }

        // Calculate dynamic buffer size: 4 (number of devices) + sum(312 + supported_interface_count * 4)
        val totalSize = 4 + exported.sumOf { dev ->
            val supportedCount = (0 until dev.interfaceCount)
                .map { dev.getInterface(it) }
                .count { isInterfaceSupported(it) }
            312 + (minOf(supportedCount, 32) * 4)
        }
        val buffer = ByteBuffer.allocate(totalSize).order(ByteOrder.BIG_ENDIAN)

        buffer.putInt(exported.size)

        for (device in exported) {
            // Match the specific handle to retrieve our assigned Bus ID
            val handle = openedDevices.values.find { it.device.deviceName == device.deviceName }
            val busId = handle?.busId ?: "1-0"

            val startPos = buffer.position()

            // path (256 bytes) - Padded
            val pathStr = "/sys/devices/virtual/usbip/$busId"
            val pathBytes = pathStr.toByteArray()
            buffer.put(pathBytes, 0, minOf(pathBytes.size, 256))
            buffer.position(startPos + 256)

            // busId (32 bytes) - Padded
            val bIdBytes = busId.toByteArray()
            buffer.put(bIdBytes, 0, minOf(bIdBytes.size, 32))
            buffer.position(startPos + 256 + 32)

            buffer.putInt(1) // bus number
            buffer.putInt(device.deviceId) // device number (transient ID)
            
            // Speed reporting (USBIP values: 1=Low, 2=Full, 3=High, 5=Super)
            // Use reflection for getSpeed() to support API 31+ while compiling against older SDKs if needed
            var speedValue = 3
            if (Build.VERSION.SDK_INT >= 31) {
                try {
                    val s = device.javaClass.getMethod("getSpeed").invoke(device) as Int
                    speedValue = when (s) {
                        1 -> 1 // LOW
                        2 -> 2 // FULL
                        3 -> 3 // HIGH
                        4 -> 5 // SUPER
                        5 -> 5 // SUPER_PLUS
                        else -> 3
                    }
                } catch (_: Exception) { }
            }
            buffer.putInt(speedValue)

            // Dynamic Hardware Attributes
            buffer.putShort((device.vendorId and 0xFFFF).toShort())
            buffer.putShort((device.productId and 0xFFFF).toShort())
            buffer.putShort(0x0111.toShort()) // bcdDevice (G29 compliant)

            buffer.put(device.deviceClass.toByte())
            buffer.put(device.deviceSubclass.toByte())
            buffer.put(device.deviceProtocol.toByte())
            buffer.put(1.toByte()) // bConfigurationValue
            buffer.put(1.toByte()) // bNumConfigurations
            
            // Strictly validate supported interface count
            val supportedInterfaces = (0 until device.interfaceCount)
                .map { device.getInterface(it) }
                .filter { isInterfaceSupported(it) }
            
            val intfCount = minOf(supportedInterfaces.size, 32)
            buffer.put(intfCount.toByte())

            // Interface Descriptors (4 bytes each)
            for (i in 0 until intfCount) {
                val interfaceDescriptor = supportedInterfaces[i]
                buffer.put(interfaceDescriptor.interfaceClass.toByte())
                buffer.put(interfaceDescriptor.interfaceSubclass.toByte())
                buffer.put(interfaceDescriptor.interfaceProtocol.toByte())
                buffer.put(0.toByte()) // padding
            }
        }

        Log.i("UsbServerService", "Generated dynamic Device List payload for ${exported.size} device(s)")
        return buffer.array()
    }

    private val usbReceiver = object : android.content.BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            }

            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_DETACHED -> {
                    device?.let { detachDevice(it) }
                }
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
                    device?.let { 
                        val profile = getDeviceProfile(it)
                        val busId = getBusId(it)
                        Log.i("UsbServerService", "USB attached: ${it.deviceName} (Bus: $busId, Profile: $profile)")
                        
                        // Auto-connect on attachment: Triggers permission prompt and handles session guard
                        handleIncomingDevice(it)
                    }
                }
                ACTION_USB_PERMISSION_SERVICE -> {
                    val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    isRequestingPermission = false
                    
                    // Retrieve the device from the intent
                    val targetDevice: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
                    } else {
                        @Suppress("DEPRECATION")
                        intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    }
                    
                    val deviceName = targetDevice?.productName ?: targetDevice?.deviceName ?: "Unknown"
                    Log.i("UsbServerService", "Permission Result: granted=$granted for $deviceName")
                    
                    if (granted && targetDevice != null) {
                        ErrorLogger.log(getString(R.string.permission_granted_msg, deviceName))
                        attachDevice(targetDevice)
                    } else {
                        Log.w("UsbServerService", "Permission DENIED for $deviceName")
                        ErrorLogger.log(getString(R.string.permission_denied_msg, deviceName))
                        
                        targetDevice?.let { usbDev ->
                            val busId = activeDeviceIdTracker[usbDev.deviceId] ?: getBusId(usbDev)
                            pendingConnections.remove(busId)
                            updateUiState()
                        }
                    }
                    processNextPermissionRequest()
                }
            }
        }
    }

    private fun getDeviceProfile(device: UsbDevice): SpecialDeviceProfile {
        val vid = device.vendorId
        val pid = device.productId

        // Valve Steam Controller (Wired or Dongle)
        if (vid == 0x28DE && (pid == 0x1302 || pid == 0x1304)) {
            return SpecialDeviceProfile.STEAM_CONTROLLER
        }

        // Logitech (0x046D)
        if (vid == 0x046D) {
            return SpecialDeviceProfile.LOGITECH_G29
        }

        // Ethernet / CDC Network - Class 0x02 (Communications) or 0xFF (Vendor Specific)
        if (device.deviceClass == 0x02 || device.deviceClass == 0xFF) {
            for (i in 0 until device.interfaceCount) {
                val intf = device.getInterface(i)
                if (intf.interfaceClass == 0x02 || intf.interfaceClass == 0x0A) return SpecialDeviceProfile.ETHERNET
            }
        }

        return SpecialDeviceProfile.GENERIC
    }

    private fun getBusId(device: UsbDevice): String {
        // Use deviceName (internal path) as a stable key for discovery
        val name = device.deviceName
        
        // 1. Check active handles
        val existingHandle = openedDevices.values.find { it.device.deviceName == name }
        if (existingHandle != null) return existingHandle.busId
        
        // 2. Check discovery cache
        val cachedId = deviceNameToBusId[name]
        if (cachedId != null) return cachedId
        
        // 3. Assign new ID and cache it
        val newId = "1-${busIdCounter++}"
        deviceNameToBusId[name] = newId
        return newId
    }

    private fun handleIncomingDevice(device: UsbDevice) {
        val profile = getDeviceProfile(device)
        val busId = getBusId(device)

        // 1. Session State Guard: Prevent duplicate port bindings or ghost sessions
        if (activeDeviceIdTracker.containsKey(device.deviceId)) {
            Log.i("UsbServerService", "Session Guard: Device ${device.deviceId} is already active. Ignoring duplicate request.")
            return
        }

        // Prevent multiple simultaneous connection attempts for the same discovery ID
        if (pendingConnections.containsKey(busId)) {
            Log.i("UsbServerService", "Session Guard: Device on bus $busId is already in pending state.")
            return
        }

        // Fetch the actual speed dynamically from Android (API 31+)
        var speedStr = "UNKNOWN / PRE-API 31"
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            try {
                val speedInt = device.javaClass.getMethod("getSpeed").invoke(device) as Int
                speedStr = when (speedInt) {
                    0 -> "UNKNOWN (0)"
                    1 -> "LOW_SPEED (1) - 1.5 Mbps"
                    2 -> "FULL_SPEED (2) - 12 Mbps"
                    3 -> "HIGH_SPEED (3) - 480 Mbps"
                    4 -> "SUPER_SPEED (4) - 5 Gbps"
                    5 -> "SUPER_SPEED_PLUS (5) - 10 Gbps / 20 Gbps"
                    else -> "UNMAPPED ($speedInt)"
                }
            } catch (_: Exception) {
                speedStr = "ERROR_FETCHING_SPEED"
            }
        }

        Log.i("UsbServerService", "Incoming USB: ${device.deviceName} (VID=${device.vendorId}, PID=${device.productId}) -> Profile: $profile | Speed: $speedStr")

        // Steam Controller firmware warning check
        if (profile == SpecialDeviceProfile.STEAM_CONTROLLER) {
            val warning = "Steam Controller Detected. Ensure 2026 firmware is installed for standard gamepad support."
            Log.w("UsbServerService", warning)
            ErrorLogger.log(warning)
            serviceScope.launch(Dispatchers.Main) {
                Toast.makeText(applicationContext, warning, Toast.LENGTH_LONG).show()
            }
        }

        // 2. Validate device for unsupported characteristics
        if (!isDeviceSupported(device)) {
            Log.w("UsbServerService", "Aborting connection: No supported interfaces found on device.")
            serviceScope.launch(Dispatchers.Main) {
                Toast.makeText(applicationContext, "This device consists exclusively of unsupported interfaces (Audio/Isochronous)", Toast.LENGTH_LONG).show()
            }
            return
        }

        // 3. Permission and Attachment Logic
        if (usbManager.hasPermission(device)) {
            pendingConnections[busId] = device.deviceId
            updateUiState()

            // Cancel any previous job for this busId to avoid race conditions
            deviceJobs.remove(busId)?.cancel()

            val job = serviceScope.launch {
                attachDevice(device)
            }
            deviceJobs[busId] = job
        } else {
            // Check if already requesting for this device
            if (!permissionQueue.any { it.deviceName == device.deviceName }) {
                permissionQueue.add(device)
                processNextPermissionRequest()
            }
        }
    }

    private fun isInterfaceSupported(intf: UsbInterface): Boolean {
        // Reject Audio Class interfaces
        if (intf.interfaceClass == UsbConstants.USB_CLASS_AUDIO) {
            return false
        }
        // Reject interfaces with any Isochronous endpoints
        for (j in 0 until intf.endpointCount) {
            val ep = intf.getEndpoint(j)
            if (ep.type == UsbConstants.USB_ENDPOINT_XFER_ISOC) {
                return false
            }
        }
        return true
    }

    private fun isDeviceSupported(device: UsbDevice): Boolean {
        // A device is supported if it has AT LEAST ONE supported interface
        for (i in 0 until device.interfaceCount) {
            val intf = device.getInterface(i)
            if (isInterfaceSupported(intf)) {
                return true
            }
        }
        Log.w("UsbServerService", "Validation Failed: Device ${device.deviceName} has no supported interfaces.")
        return false
    }

    private fun processNextPermissionRequest() {
        if (isRequestingPermission || permissionQueue.isEmpty()) return
        
        val device = permissionQueue.poll() ?: return
        isRequestingPermission = true
        
        // UI Sync: Show connecting state now that we are actually triggering the prompt
        val busId = getBusId(device)
        pendingConnections[busId] = device.deviceId
        updateUiState()
        
        requestPermissionForDevice(device)
        
        // Safety timeout for permission dialog
        serviceScope.launch {
            delay(30.seconds)
            if (isRequestingPermission) {
                isRequestingPermission = false
                Log.w("UsbServerService", "Permission request timed out for ${device.deviceName}")
                val currentBusId = activeDeviceIdTracker[device.deviceId] ?: getBusId(device)
                pendingConnections.remove(currentBusId)
                updateUiState()
                processNextPermissionRequest()
            }
        }
    }

    private fun requestPermissionForDevice(device: UsbDevice) {
        serviceScope.launch(Dispatchers.Main) {
            val intent = Intent(ACTION_USB_PERMISSION_SERVICE).apply {
                setPackage(packageName)
                putExtra(UsbManager.EXTRA_DEVICE, device)
            }
            
            // Use deviceId as requestCode to ensure unique PendingIntents without URI overhead
            val permissionIntent = android.app.PendingIntent.getBroadcast(
                this@UsbServerService, 
                device.deviceId,
                intent,
                android.app.PendingIntent.FLAG_MUTABLE or android.app.PendingIntent.FLAG_UPDATE_CURRENT
            )
            
            val name = device.productName ?: "USB Device (${device.vendorId}:${device.productId})"
            ErrorLogger.log(getString(R.string.requesting_permission_msg, name))
            usbManager.requestPermission(device, permissionIntent)
        }
    }

    private fun attachDevice(device: UsbDevice) {
        val profile = getDeviceProfile(device)
        val busId = getBusId(device)
        
        try {
            val connection = usbManager.openDevice(device)
            if (connection != null) {
                Log.i("UsbServerService", "Attaching device: ${device.productName} as $busId (Profile: $profile)")
                
                // Track active session before claiming interfaces
                activeDeviceIdTracker[device.deviceId] = busId
                
                // Special handling: Evict drivers except for Ethernet
                if (profile != SpecialDeviceProfile.ETHERNET) {
                    for (i in 0 until device.interfaceCount) {
                        // Strict null-safety and bounds-checking for composite interfaces
                        val intf = device.getInterface(i)
                        
                        // Skip unsupported interfaces (Audio/ISOC) in composite devices
                        if (!isInterfaceSupported(intf)) {
                            Log.i("UsbServerService", "Skipping unsupported interface $i (Class: ${intf.interfaceClass})")
                            continue
                        }

                        try {
                            // Force claim detaches native kernel ownership if a driver is active
                            val success = connection.claimInterface(intf, true)
                            if (success) {
                                Log.i("UsbServerService", "Successfully claimed interface $i (Class: ${intf.interfaceClass})")
                            } else {
                                // Downgrade log level for HID interfaces which are often locked by Android system
                                if (intf.interfaceClass == UsbConstants.USB_CLASS_HID) {
                                    Log.i("UsbServerService", "Interface $i (HID) is handled by Android system. Bypassing.")
                                } else {
                                    Log.w("UsbServerService", "Failed to claim interface $i (Class: ${intf.interfaceClass}) - locked by OS or driver.")
                                }
                            }
                        } catch (e: Exception) {
                            Log.e("UsbServerService", "Exception claiming interface $i: ${e.message}")
                        }
                    }
                } else {
                    Log.i("UsbServerService", "Bypassing driver eviction for Ethernet profile")
                }

                val handle = DeviceHandle(device, connection, busId, profile)
                openedDevices[busId] = handle
                
                // Notify native layer
                updateDeviceFd(busId, connection.fileDescriptor)
            } else {
                val errorMsg = "Failed to open connection for ${device.deviceName} (OS-level lock or denied)"
                Log.e("UsbServerService", errorMsg)
                ErrorLogger.log(errorMsg)
                serviceScope.launch(Dispatchers.Main) {
                    Toast.makeText(applicationContext, "Failed to open device connection (Locked by OS)", Toast.LENGTH_SHORT).show()
                }
            }
        } catch (e: Exception) {
            val errorMsg = "Crash-safe catch during device attach: ${e.message}"
            Log.e("UsbServerService", errorMsg)
            ErrorLogger.log(errorMsg, e)
            e.printStackTrace()
        } finally {
            pendingConnections.remove(busId)
            updateUiState()
        }
    }

    private fun updateUiState() {
        val uiMap = mutableMapOf<String, DeviceInfo>()
        
        // 1. Add currently opened/exported devices
        openedDevices.forEach { (busId, handle) ->
            uiMap[busId] = DeviceInfo(
                deviceId = handle.device.deviceId,
                busId = busId,
                productName = handle.device.productName ?: "Unknown Device",
                vendorId = handle.device.vendorId,
                productId = handle.device.productId,
                isConnected = true
            )
        }
        
        // 2. Add devices currently in the process of connecting
        pendingConnections.forEach { (busId, deviceId) ->
            if (!uiMap.containsKey(busId)) {
                val device = usbManager.deviceList.values.find { it.deviceId == deviceId }
                if (device != null) {
                    uiMap[busId] = DeviceInfo(
                        deviceId = deviceId,
                        busId = busId,
                        productName = device.productName ?: "Connecting...",
                        vendorId = device.vendorId,
                        productId = device.productId,
                        isConnected = false
                    )
                }
            }
        }
        
        _deviceList.value = uiMap
    }

    private fun detachDevice(device: UsbDevice) {
        // Remove from permission queue if present
        permissionQueue.removeAll { it.deviceName == device.deviceName }
        
        // Find by device name (path) or ID to ensure unified cleanup
        val busId = deviceNameToBusId[device.deviceName] ?:
                    activeDeviceIdTracker[device.deviceId] ?: 
                    openedDevices.entries.find { it.value.device.deviceName == device.deviceName }?.key ?:
                    getBusId(device)
        
        performComprehensiveCleanup(busId, device.deviceId)
    }

    private var activePerformanceSessions = 0

    @Synchronized
    @Suppress("unused")
    fun acquirePerformanceLocks() {
        activePerformanceSessions++
        if (activePerformanceSessions == 1) {
            Log.i("UsbServerService", "First session active. Acquiring performance locks.")
            wakeLock?.let {
                if (!it.isHeld) it.acquire(24 * 60 * 60 * 1000L) // 24-hour safety timeout
            }
            wifiLock?.let {
                if (!it.isHeld) it.acquire()
            }
        }
    }

    @Synchronized
    @Suppress("unused")
    fun releasePerformanceLocks() {
        if (activePerformanceSessions > 0) {
            activePerformanceSessions--
            if (activePerformanceSessions == 0) {
                Log.i("UsbServerService", "All sessions finished. Releasing performance locks.")
                wakeLock?.let {
                    if (it.isHeld) it.release()
                }
                wifiLock?.let {
                    if (it.isHeld) it.release()
                }
            }
        }
    }

    private fun forceReleasePerformanceLocks() {
        Log.i("UsbServerService", "Forcing performance locks release.")
        activePerformanceSessions = 0
        wakeLock?.let {
            if (it.isHeld) it.release()
        }
        wifiLock?.let {
            if (it.isHeld) it.release()
        }
    }

    override fun onCreate() {
        super.onCreate()
        Log.i("UsbServerService", "Service onCreate")
        usbManager = getSystemService(USB_SERVICE) as UsbManager
        usbipNsdManager = UsbipNsdManager(this)
        
        // Initialize locks but do not acquire them yet
        val powerManager = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "UsbIp:WakeLock").apply {
            setReferenceCounted(false)
        }

        // Initialize WifiLock for high performance and low latency
        val wifiManager = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
        val lockMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // API 29+: Use Low Latency mode to prevent PS-Poll throttling
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        } else {
            // Legacy fallback
            @Suppress("DEPRECATION")
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        }
        wifiLock = wifiManager.createWifiLock(lockMode, "OmniStream:WifiLock").apply {
            setReferenceCounted(false)
        }

        createNotificationChannel()

        val notification = createNotification()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
            } else {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            }
            startForeground(NOTIFICATION_ID, notification, type)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        val filter = android.content.IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_USB_PERMISSION_SERVICE)
        }
        
        ContextCompat.registerReceiver(
            this,
            usbReceiver,
            filter,
            ContextCompat.RECEIVER_EXPORTED // Required for system-triggered callbacks on some Android 14 devices
        )
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.i("UsbServerService", "Service onStartCommand")
        
        serviceScope.launch {
            nativeServerMutex.withLock {
                if (!isNativeServerStarted) {
                    Log.i("UsbServerService", "Starting persistent native server daemon")
                    val currentIp = getDeviceIpAddress(applicationContext)
                    startNativeServer(-1, currentIp)
                    isNativeServerStarted = true
                    usbipNsdManager.registerService(3240)
                }
            }

            intent?.let {
                val device: UsbDevice? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    it.getParcelableExtra("USB_DEVICE", UsbDevice::class.java)
                } else {
                    @Suppress("DEPRECATION")
                    it.getParcelableExtra("USB_DEVICE")
                }
                
                device?.let { usbDevice ->
                    // User explicitly triggered this via Intent (usually from Connect button)
                    val busId = getBusId(usbDevice)
                    authorizedBusIds.add(busId)
                    handleIncomingDevice(usbDevice)
                }
            }
        }

        return START_STICKY // Stay running
    }

    fun restartServerAndNsd() {
        try {
            val currentIp = getDeviceIpAddress(applicationContext)
            Log.i("UsbServerService", "Restarting server and NSD with strict IP: $currentIp")
            usbipNsdManager.unregisterService()
            stopNativeServer()
            startNativeServer(-1, currentIp)
            isNativeServerStarted = true
            usbipNsdManager.registerService(3240)
        } catch (e: Exception) {
            Log.e("UsbServerService", "Failed to restart server and NSD: ${e.message}")
        }
    }

    override fun onDestroy() {
        Log.i("UsbServerService", "Service onDestroy")
        unregisterReceiver(usbReceiver)
        
        forceReleasePerformanceLocks()
        wakeLock = null
        wifiLock = null

        stopNativeServer()
        usbipNsdManager.unregisterService()
        openedDevices.values.forEach { it.connection.close() }
        openedDevices.clear()
        _deviceList.value = emptyMap()
        serviceScope.cancel()
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        val serviceChannel = NotificationChannel(
            CHANNEL_ID,
            "USB/IP Server Service Channel",
            NotificationManager.IMPORTANCE_DEFAULT
        )
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(serviceChannel)
    }

    private fun createNotification(): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("USB/IP Server")
            .setContentText("Server is running in the background")
            .setSmallIcon(android.R.drawable.ic_menu_share) // Using a system icon for now
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    /**
     * A native method that is implemented by the 'usbip_server' native library.
     */
    private external fun startNativeServer(deviceFd: Int, serverIp: String)
    private external fun stopNativeServer()
    private external fun updateDeviceFd(busId: String, newFd: Int)
    private external fun invalidateDeviceFd(busId: String)

    companion object {
        private const val CHANNEL_ID = "UsbServerChannel"
        private const val NOTIFICATION_ID = 1
        private const val ACTION_USB_PERMISSION_SERVICE = "com.mizukos.usbip.USB_PERMISSION_SERVICE"

        init {
            System.loadLibrary("usbip_server")
        }
    }
}
