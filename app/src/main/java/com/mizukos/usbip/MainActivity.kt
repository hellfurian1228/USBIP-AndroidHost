package com.mizukos.usbip

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.*
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.card.MaterialCardView
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private lateinit var usbManager: UsbManager
    private lateinit var viewModel: UsbDeviceViewModel
    private lateinit var deviceAdapter: DeviceAdapter
    private lateinit var fallbackManager: SyntheticFallbackManager

    private lateinit var tvServerIp: TextView
    private lateinit var tvAllNetworks: TextView
    private lateinit var tvServiceStatus: TextView
    private lateinit var tvLogStatus: TextView
    private lateinit var rvDevices: RecyclerView
    private lateinit var tvEmptyState: TextView
    private lateinit var btnRefresh: ImageButton

    // Cache for UVC devices waiting for Camera Permission grant
    private var pendingDeviceToConnect: UsbDevice? = null

    // Modern Android 10+ Camera Permission Launcher
    private val cameraPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { isGranted ->
        if (isGranted) {
            pendingDeviceToConnect?.let { device ->
                ErrorLogger.log("Camera permission granted. Proceeding with ${device.productName}")
                viewModel.connectDevice(device)
            }
        } else {
            ErrorLogger.log("Camera permission DENIED. Cannot tunnel UVC device.")
            Toast.makeText(
                this,
                "Camera permission is required by Android to tunnel USB Scanners and Webcams.",
                Toast.LENGTH_LONG
            ).show()
        }
        pendingDeviceToConnect = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        
        // Keep screen on while app is in foreground
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        
        setContentView(R.layout.activity_main)

        // Initialize persistent log writer and crash handler
        ErrorLogger.init(this)

        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.main_root)) { v, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(v.paddingLeft, systemBars.top, v.paddingRight, systemBars.bottom)
            insets
        }

        initViews()
        setupViewModel()
        setupRecyclerView()
        observeState()

        ErrorLogger.onLogCallback = { msg ->
            viewModel.updateStatus(msg)
        }

        lifecycleScope.launch(Dispatchers.IO) {
            val appCtx = applicationContext
            val serviceIntent = Intent(appCtx, UsbServerService::class.java)
            appCtx.startForegroundService(serviceIntent)
            
            viewModel.bindService(appCtx)
            
            intent?.let { processIntent(it) }
        }
    }

    private fun initViews() {
        tvServerIp = findViewById(R.id.tv_server_ip)
        tvAllNetworks = findViewById(R.id.tv_all_networks)
        tvServiceStatus = findViewById(R.id.tv_service_status)
        tvLogStatus = findViewById(R.id.tv_log_status)
        rvDevices = findViewById(R.id.rv_devices)
        tvEmptyState = findViewById(R.id.tv_empty_state)
        val btnViewLogs: Button = findViewById(R.id.btn_view_logs)
        val btnTestInput: Button = findViewById(R.id.btn_test_input)
        btnRefresh = findViewById(R.id.btn_refresh)
        val statusCard: MaterialCardView = findViewById(R.id.status_card)
        val switchSyntheticInput: com.google.android.material.switchmaterial.SwitchMaterial = findViewById(R.id.switch_synthetic_input)

        // --- SYNTHETIC INPUT INITIALIZATION ---
        // 1. Allocate native memory for the Lock-Free SPSC queue
        SyntheticInputJni.initRingBuffer(1024)
        
        // 2. Initialize Fallback Manager
        fallbackManager = SyntheticFallbackManager(this, ErrorLogger)
        
        val rootView = findViewById<View>(R.id.main_root)

        // Test Input Button handler
        btnTestInput.setOnClickListener {
            // Instantly send a relative mouse movement (dx = 50, dy = 50)
            SyntheticInputJni.pushMouseEvent(0, 50f, 50f, 0f)

            // Simulate pressing and releasing the 'A' key (Android keycode 29)
            SyntheticInputJni.pushKeyboardEvent(29, true)  // Key down
            SyntheticInputJni.pushKeyboardEvent(29, false) // Key up

            Toast.makeText(this, "Sent test synthetic input events", Toast.LENGTH_SHORT).show()
        }

        // Handle manual UI toggle switch
        switchSyntheticInput.setOnCheckedChangeListener { _, isChecked ->
            viewModel.enableSyntheticDevice(isChecked)
            if (isChecked) {
                fallbackManager.attachSyntheticEngine(rootView)
            } else {
                fallbackManager.detachSyntheticEngine(rootView)
            }
        }

        // 3. Evaluate intercept status after UI is fully laid out (auto-enable switch if fallback detected)
        rootView.post {
            if (fallbackManager.evaluateSilentFallback(rootView)) {
                switchSyntheticInput.isChecked = true
            }
        }
        // --------------------------------------

        btnRefresh.setOnClickListener {
            lifecycleScope.launch { viewModel.refreshDevices() }
        }

        btnViewLogs.setOnClickListener {
            showDebugLogsDialog()
        }

        statusCard.setOnClickListener {
            showIpSelectionDialog()
        }
    }

    // --- GLOBAL KEYBOARD INTERCEPTOR ---
    @Suppress("RestrictedApi")
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (UsbServerService.isSyntheticInputActive) {
            // Allow system keys to bypass so the user isn't trapped
            if (event.keyCode == KeyEvent.KEYCODE_VOLUME_UP || 
                event.keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || 
                event.keyCode == KeyEvent.KEYCODE_POWER ||
                event.keyCode == KeyEvent.KEYCODE_BACK) {
                return super.dispatchKeyEvent(event)
            }

            val isDown = event.action == KeyEvent.ACTION_DOWN
            if (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP) {
                SyntheticInputJni.pushKeyboardEvent(event.keyCode, isDown)
                return true // Consume event so Android UI doesn't react
            }
        }
        return super.dispatchKeyEvent(event)
    }
    // -----------------------------------

    private fun showDebugLogsDialog() {
        val logs = ErrorLogger.getLogs()
        val message = if (logs.isEmpty()) "No debug logs captured yet." else logs
        
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Debug Logs & Diagnostics")
            .setMessage(message)
            .setPositiveButton("Close", null)
            .setNeutralButton("Copy Report") { _, _ ->
                ErrorLogger.copyLogsToClipboard(this)
            }
            .setNegativeButton("Share Zip") { _, _ ->
                ErrorLogger.shareLogArchive(this)
            }
            .show()
    }

    private fun showIpSelectionDialog() {
        val available = getAvailableIpAddresses()
        val currentSelected = NetworkPreferences.getSelectedIp(this)

        val items = mutableListOf<String>()
        items.add("Automatic (Smart Default)")
        available.forEach { ip ->
            items.add("${ip.displayName} (${ip.address}) [${ip.interfaceName}]")
        }

        val checkedItem = if (currentSelected.isNullOrEmpty()) {
            0
        } else {
            val index = available.indexOfFirst { it.address == currentSelected }
            if (index >= 0) index + 1 else 0
        }

        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("Select Server IP")
            .setSingleChoiceItems(items.toTypedArray(), checkedItem) { dialog, which ->
                if (which == 0) {
                    viewModel.setSelectedIp(this, null)
                    Toast.makeText(this, "Set to Automatic IP detection", Toast.LENGTH_SHORT).show()
                } else {
                    val selectedIp = available[which - 1].address
                    viewModel.setSelectedIp(this, selectedIp)
                    Toast.makeText(this, "Selected IP: $selectedIp", Toast.LENGTH_SHORT).show()
                }
                dialog.dismiss()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun setupViewModel() {
        usbManager = getSystemService(USB_SERVICE) as UsbManager
        viewModel = ViewModelProvider(
            this,
            object : ViewModelProvider.Factory {
            override fun <T : androidx.lifecycle.ViewModel> create(modelClass: Class<T>): T {
                @Suppress("UNCHECKED_CAST")
                return UsbDeviceViewModel(usbManager) as T
            }
        })[UsbDeviceViewModel::class.java]
    }

    private fun setupRecyclerView() {
        deviceAdapter = DeviceAdapter(
            onConnect = { info -> handleConnectionRequest(info) }
        ) { info -> viewModel.disconnectDevice(info.deviceId) }
        rvDevices.layoutManager = LinearLayoutManager(this)
        rvDevices.adapter = deviceAdapter
    }

    /**
     * Production-ready flow for Android 10-17 USB Security
     */
    private fun handleConnectionRequest(info: UsbDeviceInfo) {
        val device = usbManager.deviceList.values.find { it.deviceId == info.deviceId }
        if (device == null) {
            ErrorLogger.log("Connection failed: Hardware no longer present (ID: ${info.deviceId})")
            return
        }

        if (isUvcDevice(device)) {
            val hasCameraPerm = ContextCompat.checkSelfPermission(
                this, 
                Manifest.permission.CAMERA
            ) == PackageManager.PERMISSION_GRANTED

            if (!hasCameraPerm) {
                ErrorLogger.log("UVC Device Detected. Requesting required Camera permission...")
                pendingDeviceToConnect = device
                cameraPermissionLauncher.launch(Manifest.permission.CAMERA)
                return
            }
        }

        // Direct path for non-UVC or already authorized devices
        viewModel.connectDevice(device)
    }

    /**
     * Scans all interfaces for USB Video Class (UVC) signatures
     */
    private fun isUvcDevice(device: UsbDevice): Boolean {
        for (i in 0 until device.interfaceCount) {
            // 14 = UsbConstants.USB_CLASS_VIDEO
            if (device.getInterface(i).interfaceClass == UsbConstants.USB_CLASS_VIDEO) {
                return true
            }
        }
        return false
    }

    private fun observeState() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    viewModel.deviceIp.collectLatest { ip ->
                        tvServerIp.text = getString(R.string.ip_label, ip)
                    }
                }
                launch {
                    viewModel.availableNetworks.collectLatest { networks ->
                        if (networks.isNotEmpty()) {
                            val currentIp = viewModel.deviceIp.value
                            val text = buildString {
                                append("Available Networks:\n")
                                networks.forEach { net ->
                                    val active = if (net.address == currentIp) " (Active/Broadcasted)" else ""
                                    append("• ${net.displayName}: ${net.address}$active\n")
                                }
                            }.trim()
                            tvAllNetworks.text = text
                            tvAllNetworks.visibility = View.VISIBLE
                        } else {
                            tvAllNetworks.visibility = View.GONE
                        }
                    }
                }
                launch {
                    viewModel.statusMessage.collectLatest { msg ->
                        if (msg != null) {
                            tvLogStatus.text = msg
                            tvLogStatus.visibility = View.VISIBLE
                        }
                    }
                }
                launch {
                    viewModel.uiState.collectLatest { state ->
                        when (state) {
                            is UsbUiState.Loading -> {
                                // Could show a global progress bar if needed
                            }
                            is UsbUiState.Success -> {
                                updateDeviceList(state.devices)
                            }
                            is UsbUiState.Idle -> {}
                        }
                    }
                }
            }
        }
    }

    private fun updateDeviceList(devices: List<UsbDeviceInfo>) {
        if (devices.isEmpty()) {
            rvDevices.visibility = View.GONE
            tvEmptyState.visibility = View.VISIBLE
        } else {
            rvDevices.visibility = View.VISIBLE
            tvEmptyState.visibility = View.GONE
            deviceAdapter.submitList(devices)
        }

        val anyExported = devices.any { it.connectionState == ConnectionState.CONNECTED }
        tvServiceStatus.text = if (anyExported) getString(R.string.status_connected) else getString(R.string.status_running)
        tvServiceStatus.setTextColor(if (anyExported) getColor(R.color.colorPrimary) else getColor(R.color.colorTextSecondary))
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        lifecycleScope.launch {
            processIntent(intent)
        }
    }

    private fun processIntent(intent: Intent) {
        if (intent.action == UsbManager.ACTION_USB_DEVICE_ATTACHED) {
            val device: UsbDevice? = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.TIRAMISU) {
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            } else {
                @Suppress("DEPRECATION")
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            }
            
            if (device != null) {
                lifecycleScope.launch {
                    viewModel.refreshDevices()
                    viewModel.connectDevice(device)
                }
            }
        } else if (intent.action == UsbManager.ACTION_USB_DEVICE_DETACHED) {
            lifecycleScope.launch {
                viewModel.refreshDevices()
                viewModel.resetUiState()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        viewModel.unbindService(this)
    }

    inner class DeviceAdapter(
        private val onConnect: (UsbDeviceInfo) -> Unit,
        private val onDisconnect: (UsbDeviceInfo) -> Unit
    ) : ListAdapter<UsbDeviceInfo, DeviceViewHolder>(DeviceDiffCallback()) {

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): DeviceViewHolder {
            val view = LayoutInflater.from(parent.context).inflate(R.layout.item_device, parent, false)
            return DeviceViewHolder(view)
        }

        override fun onBindViewHolder(holder: DeviceViewHolder, position: Int) {
            holder.bind(getItem(position), onConnect, onDisconnect)
        }
    }

    class DeviceDiffCallback : DiffUtil.ItemCallback<UsbDeviceInfo>() {
        override fun areItemsTheSame(oldItem: UsbDeviceInfo, newItem: UsbDeviceInfo) =
            oldItem.deviceId == newItem.deviceId
        override fun areContentsTheSame(oldItem: UsbDeviceInfo, newItem: UsbDeviceInfo) =
            oldItem == newItem
    }

    inner class DeviceViewHolder(itemView: View) : RecyclerView.ViewHolder(itemView) {
        private val card: MaterialCardView = itemView.findViewById(R.id.device_card)
        private val tvName: TextView = itemView.findViewById(R.id.tv_device_name)
        private val tvDetails: TextView = itemView.findViewById(R.id.tv_device_details)
        private val tvStatus: TextView = itemView.findViewById(R.id.tv_connection_status)
        private val progress: ProgressBar = itemView.findViewById(R.id.progress_connecting)
        private val btnAction: Button = itemView.findViewById(R.id.btn_action)

        fun bind(device: UsbDeviceInfo, onConnect: (UsbDeviceInfo) -> Unit, onDisconnect: (UsbDeviceInfo) -> Unit) {
            tvName.text = device.deviceName
            tvDetails.text = itemView.context.getString(R.string.device_details_format, device.deviceId, device.devicePath)
            
            val isExported = device.connectionState == ConnectionState.CONNECTED
            val isConnecting = device.connectionState == ConnectionState.CONNECTING

            tvStatus.visibility = if (isExported) View.VISIBLE else View.GONE
            progress.visibility = if (isConnecting) View.VISIBLE else View.GONE
            btnAction.visibility = if (isConnecting) View.GONE else View.VISIBLE

            if (isExported) {
                card.setCardBackgroundColor(getColor(R.color.green))
                tvStatus.text = if (device.transferSpeedMbps > 0) {
                    getString(R.string.connected_active_speed, device.transferSpeedMbps)
                } else {
                    getString(R.string.connected_active)
                }
                btnAction.text = getString(R.string.disconnect)
                btnAction.setOnClickListener { onDisconnect(device) }
            } else {
                card.setCardBackgroundColor(getColor(R.color.colorSurface))
                btnAction.text = getString(R.string.connect)
                btnAction.setOnClickListener { onConnect(device) }
            }
        }
    }
}
