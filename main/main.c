/* ESP Recovery - Factory/Recovery Application
   
   This application runs from the factory partition and provides a web interface
   for partition management, recovery, and firmware updates.
*/

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// Embedded web UI (gzipped)
extern const char root_start[] asm("_binary_root_html_gz_start");
extern const char root_end[] asm("_binary_root_html_gz_end");

static const char *TAG = "esp_recovery_factory";

#define MAX_OTA_DATA_SIZE (5 * 1024 * 1024)


typedef struct {
    size_t received_bytes;
} recovery_state_t;

static recovery_state_t recovery_state = {0};

// NVS WiFi Configuration Keys
#define NVS_WIFI_NAMESPACE "wifi_config"
#define NVS_WIFI_SSID_KEY "ssid"
#define NVS_WIFI_PASSWORD_KEY "password"
#define NVS_WIFI_AUTHMODE_KEY "authmode"
// Load WiFi config from NVS, fallback to defaults if not found
static void load_wifi_config_from_nvs(wifi_config_t *wifi_config)
{
    nvs_handle_t nvs_handle;    
    esp_err_t err = nvs_open(NVS_WIFI_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    size_t ssid_len = sizeof(wifi_config->ap.ssid);
    size_t password_len = sizeof(wifi_config->ap.password);

    if (err == ESP_OK) {
        // Try to read SSID
        if (nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, (char *)wifi_config->ap.ssid, &ssid_len) != ESP_OK) {
            // Use default SSID
            strncpy((char *)wifi_config->ap.ssid, CONFIG_ESP_WIFI_SSID, ssid_len);
        }
        
        // Try to read password
        if (nvs_get_str(nvs_handle, NVS_WIFI_PASSWORD_KEY, (char *)wifi_config->ap.password, &password_len) != ESP_OK) {
            // Use default password
            strncpy((char *)wifi_config->ap.password, CONFIG_ESP_WIFI_PASSWORD, password_len);
        }
        
        // Try to read authmode
        if (nvs_get_u8(nvs_handle, NVS_WIFI_AUTHMODE_KEY, (uint8_t *)&wifi_config->ap.authmode) != ESP_OK) {
            // Use default authmode (set below)
            wifi_config->ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        }

        nvs_close(nvs_handle);
    } else {
        // NVS not available, use defaults
        strncpy((char *)wifi_config->ap.ssid, CONFIG_ESP_WIFI_SSID, ssid_len - 1);
        strncpy((char *)wifi_config->ap.password, CONFIG_ESP_WIFI_PASSWORD, password_len - 1);
        wifi_config->ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }

    wifi_config->ap.authmode = (strlen((char *)wifi_config->ap.password) > 0) ? wifi_config->ap.authmode : WIFI_AUTH_OPEN;

    
    wifi_config->ap.ssid_len = strlen((char *)wifi_config->ap.ssid);
    wifi_config->ap.max_connection = CONFIG_ESP_MAX_STA_CONN;
}

// HTTP GET Handler - Serves the UI
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const uint32_t root_len = root_end - root_start;
    ESP_LOGI(TAG, "Serving compressed UI (%u bytes)", root_len);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, root_start, root_len);
    return ESP_OK;
}

// HTTP POST Handler - Handles firmware upload
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int received = 0;
    char *buf = malloc(4096);
    int ret;

    ESP_LOGI(TAG, "Firmware upload started. Total content length: %d bytes", total_len);

    if (total_len > MAX_OTA_DATA_SIZE) {
        ESP_LOGE(TAG, "Firmware too large (%d > %d)", total_len, MAX_OTA_DATA_SIZE);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Firmware too large");
        free(buf);
        return ESP_FAIL;
    }

    const esp_partition_t *main_partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!main_partition) {
        ESP_LOGE(TAG, "Failed to find OTA partition");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA partition not found");
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (0x%x, size: 0x%x)", main_partition->label, main_partition->address, main_partition->size);

    esp_err_t err = esp_partition_erase_range(main_partition, 0, main_partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase partition");
        free(buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Partition erased successfully");

    size_t write_offset = 0;
    while (received < total_len) {
        ret = httpd_req_recv(req, buf, (total_len - received) > 4096 ? 4096 : (total_len - received));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Upload socket timeout");
            }
            break;
        }

        err = esp_partition_write(main_partition, write_offset, (const void *)buf, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write partition: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            free(buf);
            return ESP_FAIL;
        }

        received += ret;
        write_offset += ret;
        recovery_state.received_bytes += ret;
        
        if (received % (64 * 1024) == 0) {
            ESP_LOGI(TAG, "Upload progress: %d/%d bytes (%.1f%%)", received, total_len, (float)received / total_len * 100);
        }
    }

    free(buf);

    if (received != total_len) {
        ESP_LOGE(TAG, "Upload incomplete: received %d / %d bytes", received, total_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload incomplete");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Firmware uploaded successfully. Total: %d bytes", received);
    
    // Set the OTA partition as the boot partition so bootloader will use it on next reset
    err = esp_ota_set_boot_partition(main_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Boot partition set to OTA. Rebooting...");
    httpd_resp_send(req, "Firmware uploaded successfully! Device will reboot in 3 seconds.", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();

    return ESP_OK;
}

// HTTP Status Handler - Returns partition information
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *response = malloc(2048);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    char *response_ptr = response;
    int response_size = 2048;
    int remaining = response_size - 1;
    
    response_ptr += snprintf(response_ptr, remaining, "{\"partitions\":[\n");
    remaining = response_size - 1 - (response_ptr - response);
    
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    int partition_count = 0;
    
    while (it != NULL) {
        const esp_partition_t *partition = esp_partition_get(it);
        
        // Skip factory partitions
        if (partition->type == ESP_PARTITION_TYPE_APP && partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            it = esp_partition_next(it);
            continue;
        }
        
        // Include OTA and SPIFFS partitions
        bool include = false;
        if (partition->type == ESP_PARTITION_TYPE_APP && 
            (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 || 
             partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ||
             partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_2)) {
            include = true;
        }
        if (partition->type == ESP_PARTITION_TYPE_DATA && partition->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) {
            include = true;
        }
        
        if (include) {
            if (partition_count > 0) {
                response_ptr += snprintf(response_ptr, remaining, ",\n");
                remaining = response_size - 1 - (response_ptr - response);
            }
            
            response_ptr += snprintf(response_ptr, remaining,
                                    "  {\"label\":\"%s\", \"address\":\"0x%lx\", \"size\":%lu, \"type\":%d, \"subtype\":%d}",
                                    partition->label,
                                    partition->address,
                                    partition->size,
                                    partition->type,
                                    partition->subtype);
            remaining = response_size - 1 - (response_ptr - response);
            partition_count++;
        }
        
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    
    response_ptr += snprintf(response_ptr, remaining, "\n]}");
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}

// HTTP Clear Partition Handler
static esp_err_t clear_partition_handler(httpd_req_t *req)
{
    char label[64] = {0};
    char *buf = malloc(512);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, 512);
    
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        free(buf);
        return ESP_FAIL;
    }
    
    char *label_ptr = strstr(buf, "\"label\":\"");
    if (label_ptr) {
        label_ptr += strlen("\"label\":\"");
        int i = 0;
        while (label_ptr[i] != '"' && i < 63) {
            label[i] = label_ptr[i];
            i++;
        }
        label[i] = '\0';
    }
    free(buf);
    
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Clearing partition: %s", label);
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase partition");
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"Partition cleared\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP Download Partition Handler
static esp_err_t download_partition_handler(httpd_req_t *req)
{
    char label[64];
    if (httpd_req_get_url_query_str(req, label, sizeof(label)) == ESP_OK) {
        char *label_ptr = strchr(label, '=');
        if (label_ptr) {
            label_ptr++;
            memmove(label, label_ptr, strlen(label_ptr) + 1);
        }
    }
    
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Downloading partition: %s (size: %lu)", label, partition->size);
    
    char filename[128];
    snprintf(filename, sizeof(filename), "attachment; filename=\"partition_%s.bin\"", label);
    httpd_resp_set_hdr(req, "Content-Disposition", filename);
    httpd_resp_set_type(req, "application/octet-stream");
    
    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    size_t sent = 0;
    while (sent < partition->size) {
        size_t to_read = (partition->size - sent) > 4096 ? 4096 : (partition->size - sent);
        esp_err_t err = esp_partition_read(partition, sent, buf, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(err));
            free(buf);
            return ESP_FAIL;
        }
        
        if (httpd_resp_send_chunk(req, buf, to_read) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk");
            free(buf);
            return ESP_FAIL;
        }
        sent += to_read;
    }
    
    httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    
    ESP_LOGI(TAG, "Partition download complete: %zu bytes", sent);
    return ESP_OK;
}

// HTTP 404 Handler
static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "Redirect to recovery interface", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP Reset Handler
static esp_err_t reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Reset request received - rebooting device");
    httpd_resp_send(req, "Device is rebooting...", HTTPD_RESP_USE_STRLEN);
    
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    
    return ESP_OK;
}

// Start web server
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 13;
    config.lru_purge_enable = true;
    config.stack_size = 8192;  // Increase stack size to prevent overflow

    ESP_LOGI(TAG, "Starting web server on port: %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t upload = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler };
        httpd_register_uri_handler(server, &upload);
        
        httpd_uri_t status = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_register_uri_handler(server, &status);
        
        httpd_uri_t clear = { .uri = "/clear", .method = HTTP_POST, .handler = clear_partition_handler };
        httpd_register_uri_handler(server, &clear);
        
        httpd_uri_t download = { .uri = "/download", .method = HTTP_GET, .handler = download_partition_handler };
        httpd_register_uri_handler(server, &download);
        
        httpd_uri_t reset = { .uri = "/reset", .method = HTTP_POST, .handler = reset_handler };
        httpd_register_uri_handler(server, &reset);
        
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_handler);
    }
    return server;
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected - MAC: " MACSTR, MAC2STR(event->mac));
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Station disconnected");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP Recovery Factory Application ===");
    
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

    // Load WiFi config from NVS or use defaults
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    load_wifi_config_from_nvs(&wifi_config);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started - SSID: %s", (char *)wifi_config.ap.ssid);
    if (strlen((char *)wifi_config.ap.password) > 0) {
        ESP_LOGI(TAG, "Password: %s", (char *)wifi_config.ap.password);
    } else {
        ESP_LOGI(TAG, "Open network (no password)");
    }
    ESP_LOGI(TAG, "Visit http://192.168.4.1 to manage partitions");

    // Start DNS server for captive portal - redirect all DNS queries to AP IP
    dns_server_config_t dns_config = {
        .num_of_entries = 1,
        .item = {
            {
                .name = "*",
                .if_key = NULL,
                .ip = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) }
            }
        }
    };
    start_dns_server(&dns_config);

    // Start web server
    httpd_handle_t server = start_webserver();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
    }

    // Keep running - feed watchdog regularly
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        // Feed the bootloader watchdog to prevent reset to factory partition
        esp_task_wdt_reset();
    }
}
