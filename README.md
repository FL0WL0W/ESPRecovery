# ESP Recovery - Factory/Recovery Application

A web-based partition management and firmware recovery application for ESP32 devices running from the factory partition. Provides a captive portal interface and REST API for managing OTA partitions, SPIFFS, and device recovery operations.

## Features

- **Web-based UI** - Responsive, embedded gzip-compressed web interface
- **Firmware Management** - Upload, download, and manage OTA firmware partitions
- **Partition Management** - View, clear, and download any partition on the device
- **SPIFFS File Browser** - List, upload, download, and delete files with progress tracking
- **NVS Key-Value Management** - View, edit, and delete NVS keys with inline editing and auto-save
- **Boot Partition Selection** - Select which firmware partition boots on next restart
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
Returns partition information and device status including running and boot partitions.

**Response (application/json):**
```json
{
  "running_partition": "ota_0",
  "boot_partition": "ota_0",
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

### `POST /upload?label=<partition_label>`
Upload and flash binary data to any partition (APP or DATA).

**Request:** Binary data (max 5MB)
- Query Parameters:
  - `label` - Target partition label (e.g., "ota_0", "spiffs")

**Response (application/json):**
```json
{
  "status": "success",
  "message": "Binary uploaded successfully"
}
```

**Note:** Does not reboot automatically. Boot partition must be set separately with `/set_boot`.

### `POST /set_boot`
Set the boot partition for next device restart.

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
  "message": "Boot partition updated"
}
```

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

### SPIFFS File Management

#### `GET /spiffs/list?partition=<name>`
List all files in a SPIFFS partition.

**Response (application/json):**
```json
{
  "files": [
    {
      "name": "index.html",
      "size": 1024
    },
    {
      "name": "data.json",
      "size": 512
    }
  ]
}
```

#### `POST /spiffs/upload?partition=<name>&name=<filename>`
Upload a file to SPIFFS.

**Request:** Binary file data
- Query Parameters:
  - `partition` - SPIFFS partition label
  - `name` - Target filename

**Response (application/json):**
```json
{
  "status": "success",
  "message": "File uploaded"
}
```

#### `GET /spiffs/download?partition=<name>&name=<filename>`
Download a file from SPIFFS.

**Parameters:**
- `partition` - SPIFFS partition label
- `name` - Filename to download

**Response:** Binary file data (application/octet-stream)

#### `POST /spiffs/delete`
Delete a file from SPIFFS.

**Request (application/json):**
```json
{
  "partition": "spiffs",
  "name": "data.json"
}
```

**Response (application/json):**
```json
{
  "status": "success",
  "message": "File deleted"
}
```

### NVS Key-Value Management

#### `GET /nvs/list?partition=<name>`
List all keys in all namespaces from an NVS partition.

**Response (application/json):**
```json
{
  "keys": [
    {
      "namespace": "wifi_config",
      "key": "ssid",
      "type": 8,
      "value": "MyNetwork"
    },
    {
      "namespace": "app_data",
      "key": "counter",
      "type": 5,
      "value": "42"
    }
  ]
}
```

NVS types:
- 0: U8, 1: I8, 2: U16, 3: I16, 4: U32, 5: I32, 6: U64, 7: I64, 8: STR, 9: BLOB

#### `GET /nvs/get?partition=<name>&key=<key>`
Get a specific key value from NVS.

**Response (application/json):**
```json
{
  "key": "ssid",
  "type": 8,
  "value": "MyNetwork"
}
```

#### `POST /nvs/set`
Set or update an NVS key value.

**Request (application/json):**
```json
{
  "partition": "nvs",
  "namespace": "app_data",
  "key": "counter",
  "value": "100",
  "type": 5
}
```

**Response (application/json):**
```json
{
  "status": "success",
  "message": "Key updated"
}
```

**Type Field Values:**
- 0-7: Numeric types (U8, I8, U16, I16, U32, I32, U64, I64)
- 8: String (STR)
- 9: Binary (BLOB) - read-only, cannot be edited

#### `POST /nvs/delete`
Delete an NVS key.

**Request (application/json):**
```json
{
  "partition": "nvs",
  "key": "old_key"
}
```

**Response (application/json):**
```json
{
  "status": "success",
  "message": "Key deleted"
}
```

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
- **nvs** - NVS data storage for configuration

Ensure your `partitions.csv` defines these partitions appropriately.

## Web UI Features

### Partition View
- List all available partitions with address, size, and type
- See which partition is currently running (marked with ● Running indicator)
- Select boot partition with radio buttons
- Upload, download, or clear any partition

### SPIFFS Browser
- Expandable SPIFFS partition browser showing all files
- File size display
- Click filename to download
- Drag-and-drop or click to upload files with progress bar
- Quick delete with trash button
- Real-time file list updates

### NVS Key Editor
- Expandable NVS partition browser showing all keys across all namespaces
- Click any value to inline edit
- Automatic validation based on data type (numeric ranges, etc.)
- Auto-save on blur (click away)
- BLOB data shown as read-only
- Quick delete with trash button
- Real-time key list updates

### Status Ticker
- Running activity feed showing all operations
- Status messages with success/error indicators
- Auto-fade after 10 seconds
- Operation logging (uploads, deletes, boots, etc.)

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
