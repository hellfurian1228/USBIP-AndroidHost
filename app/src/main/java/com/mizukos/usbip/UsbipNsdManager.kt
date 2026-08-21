package com.mizukos.usbip

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import java.net.InetAddress

/**
 * Manages the mDNS service registration for the USBIP Host.
 * Broadcasts the presence of the USBIP server on the local network.
 */
class UsbipNsdManager(context: Context) {

    private val TAG = "UsbipNsdManager"
    private val SERVICE_TYPE = "_usbip._tcp"
    private val SERVICE_NAME = "USBIP-AndroidHost"

    private val nsdManager = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private var registrationListener: NsdManager.RegistrationListener? = null
    private var registeredServiceInfo: NsdServiceInfo? = null

    /**
     * Registers the USBIP service on the local network.
     * @param port The port on which the USBIP server is listening (default 3240).
     */
    fun registerService(port: Int) {
        if (registrationListener != null) {
            Log.w(TAG, "Service already registering or registered. Unregistering first.")
            unregisterService()
        }

        val serviceInfo = NsdServiceInfo().apply {
            serviceName = SERVICE_NAME
            serviceType = SERVICE_TYPE
            setPort(port)
            try {
                val address = InetAddress.getByName(getDeviceIpAddress())
                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                    setHostAddresses(listOf(address))
                } else {
                    @Suppress("DEPRECATION")
                    host = address
                }
            } catch (e: Exception) {
                Log.e(TAG, "Failed to set host IP: ${e.message}")
            }
        }

        registrationListener = object : NsdManager.RegistrationListener {
            override fun onServiceRegistered(NsdServiceInfo: NsdServiceInfo) {
                // Save the actual service name assigned by the system (it might have been changed to avoid conflict)
                registeredServiceInfo = NsdServiceInfo
                Log.i(TAG, "Service registered successfully: ${NsdServiceInfo.serviceName}")
            }

            override fun onRegistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "Service registration failed with error code: $errorCode")
                registrationListener = null
            }

            override fun onServiceUnregistered(arg0: NsdServiceInfo) {
                Log.i(TAG, "Service unregistered successfully: ${arg0.serviceName}")
            }

            override fun onUnregistrationFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {
                Log.e(TAG, "Service unregistration failed with error code: $errorCode")
            }
        }

        try {
            nsdManager.registerService(serviceInfo, NsdManager.PROTOCOL_DNS_SD, registrationListener)
        } catch (e: Exception) {
            Log.e(TAG, "Exception during service registration: ${e.message}")
            registrationListener = null
        }
    }

    /**
     * Unregisters the USBIP service from the local network.
     */
    fun unregisterService() {
        registrationListener?.let {
            try {
                nsdManager.unregisterService(it)
            } catch (e: Exception) {
                Log.e(TAG, "Exception during service unregistration: ${e.message}")
            }
            registrationListener = null
            registeredServiceInfo = null
        }
    }
}
