# ESP Recovery - Factory/Recovery Application

A web-based partition management and firmware recovery application for ESP32 devices running from the factory partition. Provides a captive portal interface and REST API for managing OTA partitions, SPIFFS, and device recovery operations.

## Features

- **Web-based UI** - Responsive, embedded gzip-compressed web interface
- **Firmware Management** - Upload, download, and manage OTA firmware partitions
- **Partition Management** - View, clear, and download any partition on the device
- **Captive Portal** - DNS server redirects all traffic to recovery interface
- **WiFi Configuration** - Persistent WiFi settings stored in NVS with fallback to compile-time defaults
- **Device Recovery** - One-click device reset/reboot functionality

## Configuration

WiFi settings can be configured via `menuconfig`:

```bash
idf.py menuconfig
```

Navigate to:
- **Component Config** → **OTA Updater Configuration** → WiFi Configuration
  - `CONFIG_ESP_WIFI_SSID` - AP SSID (default: "ESP32-Recovery")
  - `CONFIG_ESP_WIFI_PASSWORD` - AP password (leave empty for open network)
  - `CONFIG_ESP_MAX_STA_CONN` - Maximum concurrent connections (default: 4)

### WiFi Configuration Storage

WiFi settings are loaded in the following order:

1. **NVS Flash** - Persistent settings stored in `wifi_config` namespace
   - Keys: `ssid`, `password`, `authmode`
   - Used after first configuration via API
2. **Compile-time Defaults** - `CONFIG_ESP_WIFI_*` values from menuconfig
   - Used on first boot if NVS is not initialized

## REST API Endpoints

### `GET /`
Serves the recovery web interface (gzip-compressed HTML).

**Response:** Embedded web UI (text/html, gzip-encoded)

### `GET /status`
Returns partition information and device status.

**Response (application/json):**
```json
{
  "partitions": [
    {
      "label": "ota_0",
      "address": "0x50000",
      "size": 2097152,
      "type": 0,
      "subtype": 16
    }
  ]
}
```

### `POST /upload`
Upload and flash new firmware to OTA partition.

**Request:** Binary firmware data (max 5MB)
**Response:** 200 OK with reboot message, or error status

**Process:**
1. Validates firmware size
2. Erases target OTA partition
3. Writes firmware in 4KB chunks
4. Sets OTA partition as boot partition
5. Reboots device

### `POST /clear`
Erase a partition completely.

**Request (application/json):**
```json
{
  "label": "ota_0"
}
```

**Response (application/json):**
```json
{
  "status": "success",
  "message": "Partition cleared"
}
```

### `GET /download?label=<partition_label>`
Download partition contents as binary file.

**Parameters:**
- `label` - Partition label (e.g., "ota_0", "spiffs")

**Response:** Binary partition data (application/octet-stream)

### `POST /reset`
Trigger immediate device reboot.

**Response:** 200 OK with reboot message, device restarts after 1 second

## Network Access

Once flashed and powered on:

1. **AP Mode Active** - Device broadcasts WiFi network with configured SSID
   - SSID: Configured via `CONFIG_ESP_WIFI_SSID`
   - Password: Configured via `CONFIG_ESP_WIFI_PASSWORD`
   - IP Address: `192.168.4.1`

2. **Captive Portal** - DNS server redirects all queries to `192.168.4.1`
   - Connect to AP and any domain navigation redirects to recovery UI

3. **Access Interface**:
   - Open browser and visit: `http://192.168.4.1`
   - Or any domain (captive portal redirects)

## Development

### Project Structure

```
CMakeLists.txt           # Build configuration
main/
  main.c                 # Application logic
  root.html              # Web UI source
  CMakeLists.txt         # Component config
components/
  dns_server/            # Captive portal DNS server
```

### NVS WiFi Configuration Keys

| Key | Type | Namespace | Default |
|-----|------|-----------|---------|
| `ssid` | String | `wifi_config` | `CONFIG_ESP_WIFI_SSID` |
| `password` | String | `wifi_config` | `CONFIG_ESP_WIFI_PASSWORD` |
| `authmode` | uint8 | `wifi_config` | WPA2 (if password) / OPEN (if no password) |

### Updating WiFi Settings at Runtime

Settings can be persisted to NVS by writing to the `wifi_config` namespace using the NVS API. Future boots will use the stored configuration.

## Partition Layout

The application expects the following ESP partition table arrangement:

- **factory** - This recovery application
- **ota_0, ota_1, ota_2** - OTA update partitions
- **spiffs** - SPIFFS filesystem (optional)

Ensure your `partitions.csv` defines these partitions appropriately.

## Troubleshooting

### Device not appearing on WiFi

- Verify SSID in `menuconfig` matches expected name
- Check NVS not corrupted: Use `idf.py erase-flash` to clear everything

### Upload fails

- Ensure firmware file is valid and not corrupted
- Check device has sufficient free heap (monitor serial output)
- Firmware must be ≤ 5MB

### Web UI not loading

- Verify device is accessible at `192.168.4.1`
- Check browser has gzip decompression support
- Monitor serial output for HTTP errors
