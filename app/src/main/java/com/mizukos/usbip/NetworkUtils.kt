package com.mizukos.usbip

import android.content.Context
import androidx.core.content.edit
import java.net.Inet4Address
import java.net.NetworkInterface

data class NetworkIp(
    val interfaceName: String,
    val displayName: String,
    val address: String,
) {
    override fun toString(): String = "$displayName ($address) [$interfaceName]"
}

object NetworkPreferences {
    private const val PREF_NAME = "usbip_network_prefs"
    private const val KEY_SELECTED_IP = "selected_ip"

    fun getSelectedIp(context: Context): String? {
        val prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE)
        return prefs.getString(KEY_SELECTED_IP, null)
    }

    fun setSelectedIp(context: Context, ip: String?) {
        context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE).edit {
            putString(KEY_SELECTED_IP, ip)
        }
    }
}

/**
 * Returns a list of all available non-loopback IPv4 addresses with their interface names.
 */
fun getAvailableIpAddresses(): List<NetworkIp> {
    val list = mutableListOf<NetworkIp>()
    try {
        val interfaces = NetworkInterface.getNetworkInterfaces()
        while (interfaces.hasMoreElements()) {
            val intf = interfaces.nextElement()
            val name = intf.name ?: continue
            val displayName = intf.displayName ?: name

            if (!intf.isUp || intf.isLoopback) continue

            val addrs = intf.inetAddresses
            while (addrs.hasMoreElements()) {
                val addr = addrs.nextElement()
                if ((!addr.isLoopbackAddress) && (addr is Inet4Address)) {
                    val hostAddress = addr.hostAddress
                    if (!hostAddress.isNullOrEmpty()) {
                        list.add(NetworkIp(name, displayName, hostAddress))
                    }
                }
            }
        }
    } catch (e: Exception) {
        e.printStackTrace()
    }
    return list
}

/**
 * Intelligent IP detection:
 * 1. If user selected an IP and it's still available, return it.
 * 2. Otherwise, prioritize hotspot / Wi-Fi / tether / Ethernet interfaces over cellular.
 * 3. Fallback to first available non-loopback IPv4 or "127.0.0.1".
 */
fun getDeviceIpAddress(context: Context? = null): String {
    if (context != (null as Context?)) {
        val selected = NetworkPreferences.getSelectedIp(context!!)
        if (!selected.isNullOrEmpty()) {
            val available = getAvailableIpAddresses()
            if (available.any { it.address == selected }) {
                return selected
            }
        }
    }

    val available = getAvailableIpAddresses()
    if (available.isEmpty()) return "127.0.0.1"

    // Priority 1: Hotspot / Tether / Wi-Fi / Ethernet interfaces
    val priorityKeywords = listOf("ap", "softap", "wlan", "bridge", "rndis", "eth", "usb", "tether", "wlan1", "ap0")
    for (keyword in priorityKeywords) {
        available.find { it.interfaceName.contains(keyword, ignoreCase = true) }?.let {
            return it.address
        }
    }

    // Priority 2: Non-cellular interfaces (avoid rmnet, ccmni, pdp, wwan, mobile)
    val cellularKeywords = listOf("rmnet", "ccmni", "pdp", "wwan", "mobile", "v4-rmnet")
    val nonCellular = available.find { ip ->
        cellularKeywords.none { cell -> ip.interfaceName.contains(cell, ignoreCase = true) }
    }
    if (nonCellular != null) {
        return nonCellular.address
    }

    return available.first().address
}
