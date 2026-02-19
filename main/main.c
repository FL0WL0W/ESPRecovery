/* ESP Recovery - Factory/Recovery Application
   
   This application runs from the factory partition and provides a web interface
   for partition management, recovery, and firmware updates.
*/

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "dns_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// Embedded web UI (gzipped)
extern const char root_start[] asm("_binary_root_html_gz_start");
extern const char root_end[] asm("_binary_root_html_gz_end");

static const char *TAG = "esp_recovery_factory";

#define MAX_OTA_DATA_SIZE (5 * 1024 * 1024)

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

// HTTP POST Handler - Handles firmware/binary upload to any partition
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int received = 0;
    int ret;
    
    // Get partition label from query parameter
    char label[64] = {0};
    if (httpd_req_get_url_query_str(req, label, sizeof(label)) == ESP_OK) {
        char *label_ptr = strchr(label, '=');
        if (label_ptr) {
            label_ptr++;
            memmove(label, label_ptr, strlen(label_ptr) + 1);
            int i = 0, j = 0;
            while (label[i]) {
                if (label[i] == '%' && i + 2 < sizeof(label)) {
                    int hex;
                    sscanf(&label[i + 1], "%2x", &hex);
                    label[j++] = (char)hex;
                    i += 3;
                } else {
                    label[j++] = label[i++];
                }
            }
            label[j] = '\0';
        }
    }

    if (strlen(label) == 0) {
        ESP_LOGE(TAG, "Partition label not provided");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition label required");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Upload started. Total content length: %d bytes. Target partition: %s", total_len, label);

    if (total_len > MAX_OTA_DATA_SIZE) {
        ESP_LOGE(TAG, "Binary too large (%d > %d)", total_len, MAX_OTA_DATA_SIZE);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Binary too large");
        return ESP_FAIL;
    }

    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition) {
        ESP_LOGE(TAG, "Failed to find partition with label: %s", label);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s (0x%lx, size: 0x%lx)", partition->label, partition->address, partition->size);

    size_t write_offset = 0;
    int pages_compared = 0;
    int pages_written = 0;
    
    // Buffers for optimized writing
    char *page_buf = malloc(4096);           // 4KB buffer for reading pages
    char *existing_buf = malloc(4096);       // 4KB buffer for comparing
    char *write_buf = malloc(256 * 1024);    // 256KB accumulation buffer
    
    if (!page_buf || !existing_buf || !write_buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        if (page_buf) free(page_buf);
        if (existing_buf) free(existing_buf);
        if (write_buf) free(write_buf);
        return ESP_FAIL;
    }
    
    size_t write_buf_offset = 0;             // Offset within write_buf
    size_t write_buf_start_addr = 0;         // Starting partition address for write_buf
    bool write_buf_active = false;           // Whether we're accumulating changes
    
    while (received < total_len) {
        int to_recv = (total_len - received) > 4096 ? 4096 : (total_len - received);
        
        // Receive full 4KB or partial for last chunk
        int recv_bytes = 0;
        while (recv_bytes < to_recv) {
            ret = httpd_req_recv(req, page_buf + recv_bytes, to_recv - recv_bytes);
            if (ret <= 0) {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                    ESP_LOGE(TAG, "Upload socket timeout");
                }
                goto error_out;
            }
            recv_bytes += ret;
        }
        
        // Pad partial final page with 0xFF
        if (recv_bytes < 4096) {
            memset(page_buf + recv_bytes, 0xFF, 4096 - recv_bytes);
        }
        
        // Read existing data to compare
        esp_err_t read_err = esp_partition_read(partition, write_offset, existing_buf, 4096);
        
        // Check if this page differs
        bool data_differs = (read_err != ESP_OK) || (memcmp(page_buf, existing_buf, 4096) != 0);
        pages_compared++;
        
        if (data_differs) {
            // Page differs - add to accumulation buffer
            if (!write_buf_active) {
                write_buf_active = true;
                write_buf_start_addr = write_offset;
                write_buf_offset = 0;
            }
            
            // Copy page to write buffer
            memcpy(write_buf + write_buf_offset, page_buf, 4096);
            write_buf_offset += 4096;
            
            // Check if accumulation buffer is full or if this is the last page
            bool buf_full = (write_buf_offset >= 256 * 1024);
            bool is_last_page = (received + to_recv >= total_len);
            
            if (buf_full || is_last_page) {
                // Flush accumulation buffer
                size_t erase_len = ((write_buf_offset + 4095) / 4096) * 4096;  // Round up to 4KB
                
                esp_err_t err = esp_partition_erase_range(partition, write_buf_start_addr, erase_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to erase partition at 0x%lx: %d", write_buf_start_addr, err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase partition");
                    goto error_out;
                }
                
                err = esp_partition_write(partition, write_buf_start_addr, write_buf, write_buf_offset);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to write partition: %s", esp_err_to_name(err));
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                    goto error_out;
                }
                
                pages_written += (write_buf_offset / 4096);
                write_buf_active = false;
                write_buf_offset = 0;
            }
        } else {
            // Page matches existing - flush any pending writes
            if (write_buf_active) {
                size_t erase_len = ((write_buf_offset + 4095) / 4096) * 4096;  // Round up to 4KB
                
                esp_err_t err = esp_partition_erase_range(partition, write_buf_start_addr, erase_len);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to erase partition at 0x%lx: %d", write_buf_start_addr, err);
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to erase partition");
                    goto error_out;
                }
                
                err = esp_partition_write(partition, write_buf_start_addr, write_buf, write_buf_offset);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to write partition: %s", esp_err_to_name(err));
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
                    goto error_out;
                }
                
                pages_written += (write_buf_offset / 4096);
                write_buf_active = false;
                write_buf_offset = 0;
            }
        }
        
        write_offset += 4096;
        received += to_recv;
        
        if (received % (64 * 1024) == 0) {
            ESP_LOGI(TAG, "Upload progress: %d/%d bytes (%.1f%%) - %d/%d pages written", 
                     received, total_len, (float)received / total_len * 100, pages_written, pages_compared);
        }
    }

    free(page_buf);
    free(existing_buf);
    free(write_buf);

    if (received != total_len) {
        ESP_LOGE(TAG, "Upload incomplete: received %d / %d bytes", received, total_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload incomplete");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Binary uploaded successfully to partition '%s'. Total: %d bytes (%d pages compared, %d pages written)", 
             label, received, pages_compared, pages_written);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"Binary uploaded successfully\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;

error_out:
    free(page_buf);
    free(existing_buf);
    free(write_buf);
    return ESP_FAIL;
}

// HTTP Partition Upload Handler - Upload binary data to any partition

// HTTP Status Handler - Returns partition information
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char *response = malloc(2048);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    // Get currently running partition
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const char *running_label = running_partition ? running_partition->label : "";
    
    // Get currently selected boot partition
    const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
    const char *boot_label = boot_partition ? boot_partition->label : "";
    
    char *response_ptr = response;
    int response_size = 2048;
    int remaining = response_size - 1;
    
    response_ptr += snprintf(response_ptr, remaining, "{\"running_partition\":\"%s\", \"boot_partition\":\"%s\", \"partitions\":[\n", running_label, boot_label);
    remaining = response_size - 1 - (response_ptr - response);
    
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    int partition_count = 0;
    
    while (it != NULL) {
        const esp_partition_t *partition = esp_partition_get(it);
        
        // Include Factory, OTA, SPIFFS, and NVS partitions
        bool include = false;
        if (partition->type == ESP_PARTITION_TYPE_APP && 
            (partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ||
             (partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && partition->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX))) {
            include = true;
        }
        if (partition->type == ESP_PARTITION_TYPE_DATA && 
            (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS ||
             partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS)) {
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

// HTTP SPIFFS List Files Handler
static esp_err_t spiffs_list_handler(httpd_req_t *req)
{
    char partition_name[64] = {0};
    char mount_path[128] = {0};
    char query_buf[256] = {0};
    
    // Get partition name from query parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char *partition_ptr = strstr(query_buf, "partition=");
        if (partition_ptr) {
            partition_ptr += strlen("partition=");
            int i = 0;
            while (partition_ptr[i] != '\0' && partition_ptr[i] != '&' && i < 63) {
                partition_name[i] = partition_ptr[i];
                i++;
            }
            partition_name[i] = '\0';
        }
    }
    
    if (strlen(partition_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition name required");
        return ESP_FAIL;
    }
    
    snprintf(mount_path, sizeof(mount_path), "/%s", partition_name);
    
    // Find and mount the partition
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name);
    if (!partition) {
        ESP_LOGE(TAG, "SPIFFS partition not found: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    // Mount the partition
    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_path,
        .partition_label = partition_name,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition %s: %s", partition_name, esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to mount partition");
        return ESP_FAIL;
    }
    
    char *response = malloc(4096);
    if (!response) {
        esp_vfs_spiffs_unregister(partition_name);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int response_size = 4096;
    int written = snprintf(response, response_size, "{\"files\":[");
    
    DIR *dir = opendir(mount_path);
    if (!dir) {
        // Directory doesn't exist or is empty
        snprintf(response, response_size, "{\"files\":[]}");
    } else {
        struct dirent *entry;
        bool first = true;
        
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {  // Regular file
                struct stat file_stat;
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", mount_path, entry->d_name);
                
                if (stat(filepath, &file_stat) == 0) {
                    if (!first) {
                        snprintf(response + written, response_size - written, ",");
                        written++;
                    }
                    
                    written += snprintf(response + written, response_size - written,
                                       "{\"name\":\"%s\",\"size\":%ld}",
                                       entry->d_name, file_stat.st_size);
                    first = false;
                }
            }
        }
        closedir(dir);
        snprintf(response + written, response_size - written, "]}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    
    // Unmount the partition
    esp_vfs_spiffs_unregister(partition_name);
    
    return ESP_OK;
}

// HTTP SPIFFS Upload Handler
static esp_err_t spiffs_upload_handler(httpd_req_t *req)
{
    char filename[128] = {0};
    char partition_name[64] = {0};
    char mount_path[128] = {0};
    char *buf = malloc(512);
    
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    // Get filename and partition from query parameters
    if (httpd_req_get_url_query_str(req, buf, 512) == ESP_OK) {
        char *filename_ptr = strstr(buf, "name=");
        if (filename_ptr) {
            filename_ptr += strlen("name=");
            sscanf(filename_ptr, "%127[^&]", filename);
        }
        
        char *partition_ptr = strstr(buf, "partition=");
        if (partition_ptr) {
            partition_ptr += strlen("partition=");
            sscanf(partition_ptr, "%63[^&]", partition_name);
        }
    }
    
    if (strlen(filename) == 0 || strlen(partition_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename and partition required");
        free(buf);
        return ESP_FAIL;
    }
    
    snprintf(mount_path, sizeof(mount_path), "/%s", partition_name);
    
    // Find and mount the partition
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name);
    if (!partition) {
        ESP_LOGE(TAG, "SPIFFS partition not found: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        free(buf);
        return ESP_FAIL;
    }
    
    // Mount the partition
    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_path,
        .partition_label = partition_name,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition %s: %s", partition_name, esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to mount partition");
        free(buf);
        return ESP_FAIL;
    }
    
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", mount_path, filename);
    
    FILE *file = fopen(filepath, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filepath);
        esp_vfs_spiffs_unregister(partition_name);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        free(buf);
        return ESP_FAIL;
    }
    
    int total_len = req->content_len;
    int received = 0;
    
    ESP_LOGI(TAG, "Uploading file to SPIFFS: %s (size: %d bytes)", filepath, total_len);
    
    while (received < total_len) {
        int ret_recv = httpd_req_recv(req, buf, (total_len - received) > 512 ? 512 : (total_len - received));
        if (ret_recv <= 0) {
            if (ret_recv == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGE(TAG, "Upload socket timeout");
            }
            break;
        }
        
        if (fwrite(buf, 1, ret_recv, file) != ret_recv) {
            ESP_LOGE(TAG, "Failed to write file");
            fclose(file);
            unlink(filepath);
            esp_vfs_spiffs_unregister(partition_name);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            free(buf);
            return ESP_FAIL;
        }
        
        received += ret_recv;
    }
    
    fclose(file);
    
    if (received != total_len) {
        ESP_LOGE(TAG, "Upload incomplete: received %d / %d bytes", received, total_len);
        unlink(filepath);
        esp_vfs_spiffs_unregister(partition_name);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload incomplete");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "File uploaded successfully: %s", filepath);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"File uploaded\"}", HTTPD_RESP_USE_STRLEN);
    
    // Unmount the partition
    esp_vfs_spiffs_unregister(partition_name);
    free(buf);
    
    return ESP_OK;
}

// HTTP SPIFFS Download Handler
static esp_err_t spiffs_download_handler(httpd_req_t *req)
{
    char filename[128] = {0};
    char partition_name[64] = {0};
    char mount_path[128] = {0};
    char query_buf[512] = {0};
    
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char *filename_ptr = strstr(query_buf, "name=");
        if (filename_ptr) {
            filename_ptr += strlen("name=");
            sscanf(filename_ptr, "%127[^&]", filename);
        }
        
        char *partition_ptr = strstr(query_buf, "partition=");
        if (partition_ptr) {
            partition_ptr += strlen("partition=");
            sscanf(partition_ptr, "%63[^&]", partition_name);
        }
    }
    
    if (strlen(filename) == 0 || strlen(partition_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename and partition required");
        return ESP_FAIL;
    }
    
    snprintf(mount_path, sizeof(mount_path), "/%s", partition_name);
    
    // Find and mount the partition
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name);
    if (!partition) {
        ESP_LOGE(TAG, "SPIFFS partition not found: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    // Mount the partition
    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_path,
        .partition_label = partition_name,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition %s: %s", partition_name, esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to mount partition");
        return ESP_FAIL;
    }
    
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", mount_path, filename);
    
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filepath);
        esp_vfs_spiffs_unregister(partition_name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    ESP_LOGI(TAG, "Downloading file from SPIFFS: %s (size: %ld)", filepath, file_size);
    
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_type(req, "application/octet-stream");
    
    char *buf = malloc(4096);
    if (!buf) {
        fclose(file);
        esp_vfs_spiffs_unregister(partition_name);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    size_t read_bytes;
    while ((read_bytes = fread(buf, 1, 4096, file)) > 0) {
        if (httpd_resp_send_chunk(req, buf, read_bytes) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk");
            break;
        }
    }
    
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(file);
    free(buf);
    
    // Unmount the partition
    esp_vfs_spiffs_unregister(partition_name);
    
    ESP_LOGI(TAG, "File download complete: %s", filename);
    return ESP_OK;
}

// HTTP SPIFFS Delete Handler
static esp_err_t spiffs_delete_handler(httpd_req_t *req)
{
    char filename[128] = {0};
    char partition_name[64] = {0};
    char mount_path[128] = {0};
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
    
    // Parse JSON to get filename and partition
    char *filename_ptr = strstr(buf, "\"name\":\"");
    if (filename_ptr) {
        filename_ptr += strlen("\"name\":\"");
        int i = 0;
        while (filename_ptr[i] != '"' && i < 127) {
            filename[i] = filename_ptr[i];
            i++;
        }
        filename[i] = '\0';
    }
    
    char *partition_ptr = strstr(buf, "\"partition\":\"");
    if (partition_ptr) {
        partition_ptr += strlen("\"partition\":\"");
        int i = 0;
        while (partition_ptr[i] != '"' && i < 63) {
            partition_name[i] = partition_ptr[i];
            i++;
        }
        partition_name[i] = '\0';
    }
    free(buf);
    
    if (strlen(filename) == 0 || strlen(partition_name) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename and partition required");
        return ESP_FAIL;
    }
    
    snprintf(mount_path, sizeof(mount_path), "/%s", partition_name);
    
    // Find and mount the partition
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, partition_name);
    if (!partition) {
        ESP_LOGE(TAG, "SPIFFS partition not found: %s", partition_name);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    // Mount the partition
    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_path,
        .partition_label = partition_name,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    
    esp_err_t ret_mount = esp_vfs_spiffs_register(&conf);
    if (ret_mount != ESP_OK && ret_mount != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS partition %s: %s", partition_name, esp_err_to_name(ret_mount));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to mount partition");
        return ESP_FAIL;
    }
    
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", mount_path, filename);
    
    ESP_LOGI(TAG, "Deleting file: %s", filepath);
    
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", filepath);
        esp_vfs_spiffs_unregister(partition_name);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
        return ESP_FAIL;
    }
    
    // Unmount the partition
    esp_vfs_spiffs_unregister(partition_name);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"File deleted\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP NVS List Handler - Lists all keys in all namespaces in an NVS partition
static esp_err_t nvs_list_handler(httpd_req_t *req)
{
    char partition_name[64] = {0};
    char query_buf[256] = {0};
    
    // Get partition name from query parameter
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        ESP_LOGI(TAG, "Query string: %s", query_buf);
        char *partition_ptr = strstr(query_buf, "partition=");
        if (partition_ptr) {
            partition_ptr += strlen("partition=");
            int i = 0;
            while (partition_ptr[i] != '\0' && partition_ptr[i] != '&' && i < 63) {
                partition_name[i] = partition_ptr[i];
                i++;
            }
            partition_name[i] = '\0';
        }
    }
    
    ESP_LOGI(TAG, "NVS list handler - extracted partition name: '%s' (len: %d)", partition_name, strlen(partition_name));
    
    if (strlen(partition_name) == 0) {
        ESP_LOGE(TAG, "No partition name provided");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition name required");
        return ESP_FAIL;
    }
    
    char *response = malloc(16384);
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int written = snprintf(response, 16384, "{\"keys\":[");
    
    // Iterate through ALL namespaces and keys
    nvs_iterator_t it = NULL;
    esp_err_t iter_err = nvs_entry_find(partition_name, NULL, NVS_TYPE_ANY, &it);
    
    ESP_LOGI(TAG, "Starting NVS iteration for partition: %s, entry_find result: %s", partition_name, esp_err_to_name(iter_err));
    
    bool first = true;
    
    while (it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        
        ESP_LOGI(TAG, "Found NVS entry - namespace: '%s', key: '%s', type: %d", info.namespace_name, info.key, info.type);
        
        if (!first) {
            written += snprintf(response + written, 16384 - written, ",");
        }
        
        // Open handle for this specific namespace to read the value
        nvs_handle_t handle = 0;
        esp_err_t open_err = nvs_open_from_partition(partition_name, info.namespace_name, NVS_READONLY, &handle);
        
        char value_str[512] = {0};
        if (open_err == ESP_OK) {
            // Get value based on type
            if (info.type == NVS_TYPE_I8) {
                int8_t val;
                if (nvs_get_i8(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%d", val);
                }
            } else if (info.type == NVS_TYPE_U8) {
                uint8_t val;
                if (nvs_get_u8(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%u", val);
                }
            } else if (info.type == NVS_TYPE_I16) {
                int16_t val;
                if (nvs_get_i16(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%d", val);
                }
            } else if (info.type == NVS_TYPE_U16) {
                uint16_t val;
                if (nvs_get_u16(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%u", val);
                }
            } else if (info.type == NVS_TYPE_I32) {
                int32_t val;
                if (nvs_get_i32(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%ld", val);
                }
            } else if (info.type == NVS_TYPE_U32) {
                uint32_t val;
                if (nvs_get_u32(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%lu", val);
                }
            } else if (info.type == NVS_TYPE_I64) {
                int64_t val;
                if (nvs_get_i64(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%lld", val);
                }
            } else if (info.type == NVS_TYPE_U64) {
                uint64_t val;
                if (nvs_get_u64(handle, info.key, &val) == ESP_OK) {
                    snprintf(value_str, sizeof(value_str), "%llu", val);
                }
            } else if (info.type == NVS_TYPE_STR) {
                size_t len = 512;
                char str_val[512] = {0};
                if (nvs_get_str(handle, info.key, str_val, &len) == ESP_OK) {
                    // Escape quotes and backslashes in string values
                    int idx = 0, out_idx = 0;
                    while (str_val[idx] && out_idx < 500) {
                        if (str_val[idx] == '"' || str_val[idx] == '\\') {
                            value_str[out_idx++] = '\\';
                        }
                        value_str[out_idx++] = str_val[idx++];
                    }
                    value_str[out_idx] = '\0';
                }
            } else if (info.type == NVS_TYPE_BLOB) {
                snprintf(value_str, sizeof(value_str), "[BLOB data]");
            }
            
            nvs_close(handle);
        } else {
            snprintf(value_str, sizeof(value_str), "[Error opening namespace]");
        }
        
        written += snprintf(response + written, 16384 - written,
                           "{\"namespace\":\"%s\",\"key\":\"%s\",\"type\":%d,\"value\":\"%s\"}",
                           info.namespace_name, info.key, info.type, value_str);
        
        if (nvs_entry_next(&it) != ESP_OK) {
            break;
        }
        first = false;
    }
    
    snprintf(response + written, 16384 - written, "]}");
    
    ESP_LOGI(TAG, "NVS list response size: %d bytes", strlen(response));
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    return ESP_OK;
}

// HTTP NVS Get Handler - Get a specific key value
static esp_err_t nvs_get_handler(httpd_req_t *req)
{
    char partition_name[64] = {0};
    char key[64] = {0};
    char query_buf[512] = {0};
    
    // Get partition name and key from query parameters
    if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
        char *partition_ptr = strstr(query_buf, "partition=");
        if (partition_ptr) {
            partition_ptr += strlen("partition=");
            int i = 0;
            while (partition_ptr[i] != '\0' && partition_ptr[i] != '&' && i < 63) {
                partition_name[i] = partition_ptr[i];
                i++;
            }
            partition_name[i] = '\0';
        }
        
        char *key_ptr = strstr(query_buf, "key=");
        if (key_ptr) {
            key_ptr += strlen("key=");
            int i = 0;
            while (key_ptr[i] != '\0' && key_ptr[i] != '&' && i < 63) {
                key[i] = key_ptr[i];
                i++;
            }
            key[i] = '\0';
        }
    }
    
    if (strlen(partition_name) == 0 || strlen(key) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition and key required");
        return ESP_FAIL;
    }
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(partition_name, "", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open NVS");
        return ESP_FAIL;
    }
    
    // Try to get the value - attempt all types
    char response[1024] = {0};
    nvs_iterator_t it = NULL;
    esp_err_t iter_err = nvs_entry_find(partition_name, "", NVS_TYPE_ANY, &it);
    bool found = false;
    
    while (it != NULL && !found) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        
        if (strcmp(info.key, key) == 0) {
            char value_str[512] = {0};
            
            if (info.type == NVS_TYPE_STR) {
                size_t len = 512;
                char str_val[512] = {0};
                nvs_get_str(handle, info.key, str_val, &len);
                snprintf(value_str, sizeof(value_str), "%s", str_val);
            } else if (info.type == NVS_TYPE_BLOB) {
                snprintf(value_str, sizeof(value_str), "[Binary data]");
            } else if (info.type == NVS_TYPE_I32) {
                int32_t val;
                nvs_get_i32(handle, info.key, &val);
                snprintf(value_str, sizeof(value_str), "%ld", val);
            } else if (info.type == NVS_TYPE_U32) {
                uint32_t val;
                nvs_get_u32(handle, info.key, &val);
                snprintf(value_str, sizeof(value_str), "%lu", val);
            }
            
            snprintf(response, sizeof(response), "{\"key\":\"%s\",\"type\":%d,\"value\":\"%s\"}", key, info.type, value_str);
            found = true;
        }
        
        nvs_entry_next(&it);
    }
    nvs_close(handle);
    
    if (!found) {
        snprintf(response, sizeof(response), "{\"error\":\"Key not found\"}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// HTTP NVS Delete Handler
static esp_err_t nvs_delete_handler(httpd_req_t *req)
{
    char partition_name[64] = {0};
    char key[64] = {0};
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
    
    // Parse JSON
    char *partition_ptr = strstr(buf, "\"partition\":\"");
    if (partition_ptr) {
        partition_ptr += strlen("\"partition\":\"");
        int i = 0;
        while (partition_ptr[i] != '"' && i < 63) {
            partition_name[i] = partition_ptr[i];
            i++;
        }
        partition_name[i] = '\0';
    }
    
    char *key_ptr = strstr(buf, "\"key\":\"");
    if (key_ptr) {
        key_ptr += strlen("\"key\":\"");
        int i = 0;
        while (key_ptr[i] != '"' && i < 63) {
            key[i] = key_ptr[i];
            i++;
        }
        key[i] = '\0';
    }
    free(buf);
    
    if (strlen(partition_name) == 0 || strlen(key) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition and key required");
        return ESP_FAIL;
    }
    
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(partition_name, "", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open NVS");
        return ESP_FAIL;
    }
    
    esp_err_t del_err = nvs_erase_key(handle, key);
    if (del_err != ESP_OK) {
        nvs_close(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete key");
        return ESP_FAIL;
    }
    
    nvs_commit(handle);
    nvs_close(handle);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"Key deleted\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP NVS Set Handler - Updates a key value in NVS partition
static esp_err_t nvs_set_handler(httpd_req_t *req)
{
    char partition_name[64] = {0};
    char namespace_name[64] = {0};
    char key[64] = {0};
    char value[512] = {0};
    int type = -1;
    char *buf = malloc(1024);
    
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }
    
    int ret = httpd_req_recv(req, buf, 1024);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        free(buf);
        return ESP_FAIL;
    }
    
    // Parse JSON
    char *partition_ptr = strstr(buf, "\"partition\":\"");
    if (partition_ptr) {
        partition_ptr += strlen("\"partition\":\"");
        int i = 0;
        while (partition_ptr[i] != '"' && i < 63) {
            partition_name[i] = partition_ptr[i];
            i++;
        }
        partition_name[i] = '\0';
    }
    
    char *namespace_ptr = strstr(buf, "\"namespace\":\"");
    if (namespace_ptr) {
        namespace_ptr += strlen("\"namespace\":\"");
        int i = 0;
        while (namespace_ptr[i] != '"' && i < 63) {
            namespace_name[i] = namespace_ptr[i];
            i++;
        }
        namespace_name[i] = '\0';
    }
    
    char *key_ptr = strstr(buf, "\"key\":\"");
    if (key_ptr) {
        key_ptr += strlen("\"key\":\"");
        int i = 0;
        while (key_ptr[i] != '"' && i < 63) {
            key[i] = key_ptr[i];
            i++;
        }
        key[i] = '\0';
    }
    
    char *value_ptr = strstr(buf, "\"value\":\"");
    if (value_ptr) {
        value_ptr += strlen("\"value\":\"");
        int i = 0;
        while (value_ptr[i] != '"' && i < 510) {
            if (value_ptr[i] == '\\' && value_ptr[i+1] == '"') {
                value[i] = '"';
                value_ptr++;
            } else {
                value[i] = value_ptr[i];
            }
            i++;
        }
        value[i] = '\0';
    }
    
    char *type_ptr = strstr(buf, "\"type\":");
    if (type_ptr) {
        type_ptr += strlen("\"type\":");
        sscanf(type_ptr, "%d", &type);
    }
    
    free(buf);
    
    if (strlen(partition_name) == 0 || strlen(key) == 0 || type < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition, key, and type required");
        return ESP_FAIL;
    }
    
    // Open NVS partition with the specified namespace
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(partition_name, namespace_name, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS partition '%s' namespace '%s': %s", partition_name, namespace_name, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open NVS");
        return ESP_FAIL;
    }
    
    // Write value based on type
    esp_err_t write_err = ESP_OK;
    switch (type) {
        case 0: { // U8
            uint8_t val = (uint8_t)atoi(value);
            write_err = nvs_set_u8(handle, key, val);
            break;
        }
        case 1: { // I8
            int8_t val = (int8_t)atoi(value);
            write_err = nvs_set_i8(handle, key, val);
            break;
        }
        case 2: { // U16
            uint16_t val = (uint16_t)atoi(value);
            write_err = nvs_set_u16(handle, key, val);
            break;
        }
        case 3: { // I16
            int16_t val = (int16_t)atoi(value);
            write_err = nvs_set_i16(handle, key, val);
            break;
        }
        case 4: { // U32
            uint32_t val = (uint32_t)strtoul(value, NULL, 10);
            write_err = nvs_set_u32(handle, key, val);
            break;
        }
        case 5: { // I32
            int32_t val = (int32_t)strtol(value, NULL, 10);
            write_err = nvs_set_i32(handle, key, val);
            break;
        }
        case 6: { // U64
            uint64_t val = (uint64_t)strtoull(value, NULL, 10);
            write_err = nvs_set_u64(handle, key, val);
            break;
        }
        case 7: { // I64
            int64_t val = (int64_t)strtoll(value, NULL, 10);
            write_err = nvs_set_i64(handle, key, val);
            break;
        }
        case 8: { // STR
            write_err = nvs_set_str(handle, key, value);
            break;
        }
        case 9: { // BLOB
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot edit BLOB data");
            nvs_close(handle);
            return ESP_FAIL;
        }
        default:
            write_err = ESP_ERR_NVS_TYPE_MISMATCH;
    }
    
    if (write_err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed to write NVS key: %s", esp_err_to_name(write_err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write key");
        return ESP_FAIL;
    }
    
    nvs_commit(handle);
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Successfully updated NVS key '%s' in namespace '%s' partition '%s'", key, namespace_name, partition_name);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"Key updated\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP Set Boot Partition Handler
static esp_err_t set_boot_partition_handler(httpd_req_t *req)
{
    char partition_label[64] = {0};
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
    
    // Parse JSON to get partition label
    char *label_ptr = strstr(buf, "\"label\":\"");
    if (label_ptr) {
        label_ptr += strlen("\"label\":\"");
        int i = 0;
        while (label_ptr[i] != '"' && i < 63) {
            partition_label[i] = label_ptr[i];
            i++;
        }
        partition_label[i] = '\0';
    }
    free(buf);
    
    if (strlen(partition_label) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Partition label required");
        return ESP_FAIL;
    }
    
    // Find the partition
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (!partition) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Partition not found");
        return ESP_FAIL;
    }
    
    // Set the boot partition
    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Boot partition set to: %s", partition_label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"success\", \"message\":\"Boot partition updated\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Start web server
static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 21;  // Increase to accommodate all URI handlers
    config.max_open_sockets = 13;
    config.lru_purge_enable = true;
    config.stack_size = 8192;  // Increase stack size to prevent overflow

    ESP_LOGI(TAG, "Starting web server on port: %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t upload = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler };
        httpd_register_uri_handler(server, &upload);
        
        // Register general download handler
        httpd_uri_t download = { .uri = "/download", .method = HTTP_GET, .handler = download_partition_handler };
        httpd_register_uri_handler(server, &download);

        httpd_uri_t status = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler };
        httpd_register_uri_handler(server, &status);
        
        httpd_uri_t clear = { .uri = "/clear", .method = HTTP_POST, .handler = clear_partition_handler };
        httpd_register_uri_handler(server, &clear);
        
        // Register boot partition handler
        httpd_uri_t set_boot = { .uri = "/set_boot", .method = HTTP_POST, .handler = set_boot_partition_handler };
        httpd_register_uri_handler(server, &set_boot);

        httpd_uri_t reset = { .uri = "/reset", .method = HTTP_POST, .handler = reset_handler };
        httpd_register_uri_handler(server, &reset);
        
        // Register SPIFFS handlers BEFORE general download handler (more specific paths first)
        httpd_uri_t spiffs_list = { .uri = "/spiffs/list", .method = HTTP_GET, .handler = spiffs_list_handler };
        httpd_register_uri_handler(server, &spiffs_list);
        
        httpd_uri_t spiffs_upload = { .uri = "/spiffs/upload", .method = HTTP_POST, .handler = spiffs_upload_handler };
        httpd_register_uri_handler(server, &spiffs_upload);
        
        httpd_uri_t spiffs_download = { .uri = "/spiffs/download", .method = HTTP_GET, .handler = spiffs_download_handler };
        httpd_register_uri_handler(server, &spiffs_download);
        
        httpd_uri_t spiffs_delete = { .uri = "/spiffs/delete", .method = HTTP_POST, .handler = spiffs_delete_handler };
        httpd_register_uri_handler(server, &spiffs_delete);
        
        // Register NVS handlers
        httpd_uri_t nvs_list = { .uri = "/nvs/list", .method = HTTP_GET, .handler = nvs_list_handler };
        httpd_register_uri_handler(server, &nvs_list);
        
        httpd_uri_t nvs_get = { .uri = "/nvs/get", .method = HTTP_GET, .handler = nvs_get_handler };
        httpd_register_uri_handler(server, &nvs_get);
        
        httpd_uri_t nvs_delete = { .uri = "/nvs/delete", .method = HTTP_POST, .handler = nvs_delete_handler };
        httpd_register_uri_handler(server, &nvs_delete);
        
        httpd_uri_t nvs_set = { .uri = "/nvs/set", .method = HTTP_POST, .handler = nvs_set_handler };
        httpd_register_uri_handler(server, &nvs_set);
        
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
    esp_netif_ip_info_t ip_info = {
        .ip = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
        .gw = { .addr = ESP_IP4TOADDR(0, 0, 0, 0) },  // No gateway
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) }
    };
    esp_netif_inherent_config_t esp_netif_inherent_ap_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    esp_netif_inherent_ap_config.ip_info = &ip_info;
    esp_netif_t *ap_netif = esp_netif_create_wifi(WIFI_IF_AP, &esp_netif_inherent_ap_config);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());

    // Set captive portal URI (DHCP Option 114) for devices that support it
    const char *captive_portal_uri = "http://192.168.4.1/";
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, 
                                            ESP_NETIF_CAPTIVEPORTAL_URI, 
                                            (void *)captive_portal_uri, strlen(captive_portal_uri)));

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
