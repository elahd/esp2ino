#include "esp2ino_common.h"

static bool sysInfoTransmitted = false;
char httpRespBuffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4))) = {0};

static esp_err_t handleRoot(httpd_req_t *req);
static esp_err_t handleFlash(httpd_req_t *req);
static esp_err_t handleBackup(httpd_req_t *req);
static esp_err_t handleInfo(httpd_req_t *req);
static esp_err_t handleUpload(httpd_req_t *req);
static esp_err_t handleWifi(httpd_req_t *req);
static esp_err_t handleWifiStatus(httpd_req_t *req);
static esp_err_t handleSafeMode(httpd_req_t *req);
static int req_getClientIp(httpd_req_t *req, char *ipstr);

/**
 * URI Handlers
 */
httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handleRoot};
httpd_uri_t flash = {.uri = "/flash", .method = HTTP_GET, .handler = handleFlash};
httpd_uri_t status = {.uri = "/info", .method = HTTP_GET, .handler = handleInfo};
httpd_uri_t backup = {.uri = "/backup", .method = HTTP_GET, .handler = handleBackup};
httpd_uri_t undo = {.uri = "/undo", .method = HTTP_GET, .handler = handleUndo};
httpd_uri_t upload = {.uri = "/upload", .method = HTTP_PUT, .handler = handleUpload};
httpd_uri_t wifi = {.uri = "/wifi", .method = HTTP_GET, .handler = handleWifi};
httpd_uri_t wifiStatus = {.uri = "/wifiStatus", .method = HTTP_GET, .handler = handleWifiStatus};
httpd_uri_t safeMode = {.uri = "/safeMode", .method = HTTP_GET, .handler = handleSafeMode};

/**
 * Handler Functions
 */
static esp_err_t handleRoot(httpd_req_t *req)
{
    // static const char *TAG = "server__handleRoot";

    // Reloading the entire page. sysInfo needs to re-initialize everything.
    sysInfoTransmitted = false;

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, index_html_gz, index_html_gz_len);

    return ESP_OK;
}

static esp_err_t handleSafeMode(httpd_req_t *req)
{

    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t handleUpload(httpd_req_t *req)
{
    static const char *TAG = "server__handleUpload";

    memset(httpRespBuffer, 0, sizeof httpRespBuffer);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_status(req, HTTPD_200);

    if (doFlash(req, OTA_TYPE_UPLOAD) == ESP_OK)
    {

        flashSuccessWrapUp(req);

        /** Close HTTP Response **/
        httpd_resp_send_chunk(req, NULL, 0);

        esp_restart();
    }
    else
    {
        sprintf(httpRespBuffer, "Flashing failed.");
        ESP_LOGE(TAG, "%s", httpRespBuffer);

#if FEAT_DEBUG_LOG_TO_WEBUI
        /** Disable Log to Web **/
        esp_log_set_putchar(old_logger);
#endif

        /** Close HTTP Response **/
        httpd_resp_send_chunk(req, NULL, 0);

        return ESP_OK;
    }
}

/**
 * This function retrieves flash size from image headers, not from SPI chip
 * itself.
 * */
static esp_err_t handleBackup(httpd_req_t *req)
{
    static const char *TAG = "server__handleBackup";

    ESP_LOGI(TAG, "BEGIN: Sending backup.");

    char buf[BACKUP_BUF_SIZE];
    char hdr_disp_buf[150];
    char hdr_len_buf[150];

    sprintf(hdr_disp_buf, "attachment; filename=\"firmware-%s.bin\"", sys_mac_str);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr_disp_buf);

    sprintf(hdr_len_buf, "%d", sys_realFlashSize);
    httpd_resp_set_hdr(req, "Content-Length", hdr_len_buf);

    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);

    int read = 0;
    while (read < sys_realFlashSize)
    {
        spi_flash_read(read, (char *)buf, BACKUP_BUF_SIZE);
        httpd_resp_send_chunk(req, buf, BACKUP_BUF_SIZE);
        read += BACKUP_BUF_SIZE;
    }

    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "DONE: Sending backup.");

    return ESP_OK;
}

static esp_err_t handleWifiStatus(httpd_req_t *req)
{
    // static const char *TAG = "server__handleWifiStatus";

    char *jsonBuffer = malloc(512);

    // Hold if Wi-Fi status is requested by webui while connection attempts in progress.
    if (wifi__connectSemaphore != NULL)
    {
        xSemaphoreTake(wifi__connectSemaphore, (TickType_t)(10000 / portTICK_PERIOD_MS));
    }

    if (strncmp(wifi__staSsid, "", sizeof("")))
    {
        snprintf(jsonBuffer, 1024,
                 "{"
                 "\"status\":\"connected\","
                 "\"ssid\":\"%s\","
                 "\"channel\":\"%s\","
                 "\"bssid\":\"" MACSTR "\""
                 "}",
                 wifi__staSsid,
                 wifi__channel,
                 MAC2STR(wifi__staBssid));
    }
    else
    {
        snprintf(jsonBuffer, 512, "{\"status\":\"not_connected\"}");
    }

    httpd_resp_send(req, jsonBuffer, -1);

    free(jsonBuffer);

    xSemaphoreGive(wifi__connectSemaphore);

    return ESP_OK;
}

static esp_err_t handleFlash(httpd_req_t *req)
{
    static const char *TAG = "server__handleFlash";

    memset(httpRespBuffer, 0, sizeof httpRespBuffer);
    char *buf;
    size_t buf_len;

    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_status(req, HTTPD_200);

#if FEAT_DEBUG_LOG_TO_WEBUI
    // Make http request available to webui_debugPub.
    flash_httpdReq = req;
#endif

    char client_ipstr[INET6_ADDRSTRLEN];
    req_getClientIp(req, client_ipstr);

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {
            ESP_LOGI(TAG, "Found query parameters => %s", buf);
            // Get URL parameter
            if (httpd_query_key_value(buf, "url", user_url_buf,
                                      sizeof(user_url_buf)) == ESP_OK)
            {
                ESP_LOGI(TAG, "Found URL query parameter => url=%s", user_url_buf);
            }
        }
        free(buf);
    }

#if FEAT_DEBUG_LOG_TO_WEBUI
    /** Enable Log to Web **/
    old_logger = esp_log_set_putchar(&webui_debugPub);
#endif

    if (user_url_buf == 0 || strcmp(user_url_buf, "") == 0)
    {
        ESP_LOGW(TAG, "No firmware URL provided. Nothing to flash.");

        // TO-DO: Send an error that can be processed by the web ui.
        httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));

#if FEAT_DEBUG_LOG_TO_WEBUI
        /** Disable Log to Web **/
        esp_log_set_putchar(old_logger);
        esp_log_level_set(TAG, ESP_LOG_INFO);
#endif

        /** Close HTTP Response **/
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send_chunk(req, NULL, 0);

        return ESP_OK;
    }

    ESP_LOGI(TAG, "Using URL: %s", user_url_buf);

    if (doFlash(req, OTA_TYPE_DOWNLOAD) == ESP_OK)
    {

        flashSuccessWrapUp(req);

        xEventGroupSetBits(rebootAgent_EventGroup, REBOOT_NOW_BIT);
    }
    else
    {
        sprintf(httpRespBuffer, "Flashing %s failed.", user_url_buf);
        ESP_LOGE(TAG, "%s", httpRespBuffer);

#if FEAT_DEBUG_LOG_TO_WEBUI
        /** Disable Log to Web **/
        esp_log_set_putchar(old_logger);
#endif
    }

    /** Close HTTP Response **/
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

esp_err_t handleUndo(httpd_req_t *req)
{
    static const char *TAG = "server__handleUndo";

    memset(httpRespBuffer, 0, sizeof httpRespBuffer);

    if (flash_erasedFactoryApp)
    {
        sprintf(httpRespBuffer, "Can't undo. The factory app no longer exists.");
        ESP_LOGW(TAG, "%s", httpRespBuffer);
    }
    else
    {
        esp_err_t ret = esp_ota_set_boot_partition(sys_partIdle);
        if (ret)
        {
            sprintf(httpRespBuffer, "Failed to undo.");
            ESP_LOGE(TAG, "%s", httpRespBuffer);
        }
        else
        {
            sys_partIdle = esp_ota_get_next_update_partition(NULL);
            sprintf(httpRespBuffer, "Undone successfully. Power cycle your device to return to factory firmware.");
            ESP_LOGI(TAG, "%s", httpRespBuffer);
        }
    }

    httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));

    return ESP_OK;
}

static esp_err_t handleWifi(httpd_req_t *req)
{
    static const char *TAG = "server__handleWifi";

    memset(httpRespBuffer, 0, sizeof httpRespBuffer);

    char *buf;
    size_t buf_len;
    bool wifi_set = false;

    char param_ssid[32];
    char param_pswd[64];

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1)
    {
        buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK)
        {

            ESP_LOGI(TAG, "Found query parameters => %s", buf);
            // Get SSID
            if (httpd_query_key_value(buf, "ssid", param_ssid,
                                      sizeof(param_ssid)) == ESP_OK)
            {
                char *buf_ssid = url_decode(param_ssid);
                strlcpy(param_ssid, buf_ssid, sizeof(param_ssid));
                free(buf_ssid);

                ESP_LOGI(TAG, "Found URL query parameter => ssid=%s", param_ssid);
                wifi_set = true;
            }
            // Get Password (Password is optional.)
            if (httpd_query_key_value(buf, "pswd", param_pswd,
                                      sizeof(param_pswd)) == ESP_OK)
            {
                char *buf_pswd = url_decode(param_pswd);
                strlcpy(param_pswd, buf_pswd, sizeof(param_pswd));
                free(buf_pswd);

                ESP_LOGI(TAG, "Found URL query parameter => pswd=%s", param_pswd);
            }
        }
        free(buf);
    }

    if (wifi_set)
    {
        if (wifi__connectSemaphore != NULL)
        {
            xSemaphoreTake(wifi__connectSemaphore, (TickType_t)(10000 / portTICK_PERIOD_MS));
        }
        if (wifi__apStaSetHelper(req, param_ssid, param_pswd) != ESP_OK)
        {
            char *ret = "{\"type\":\"error\",\"code\":\"wifi_set_failed\"}";
            httpd_resp_send_chunk(req, ret, -1);
        }
    }
    else
    {
        // Scan and return results.
        if (wifi__apStaScanHelper(req, httpRespBuffer, sizeof(httpRespBuffer)) != ESP_OK)
        {
            char *ret = "{\"type\":\"error\",\"code\":\"wifi_scan_failed\"}";
            httpd_resp_send_chunk(req, ret, -1);
        }
    }

    /** Close HTTP Response **/
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handleInfo(httpd_req_t *req)
{
    memset(httpRespBuffer, 0, sizeof httpRespBuffer);

    char client_ipstr[INET6_ADDRSTRLEN];
    req_getClientIp(req, client_ipstr);

    const char *FlashSize = "";
    switch (flash_data[3] & 0xF0)
    {
    case 0x00:
        FlashSize = "512K";
        break;
    case 0x10:
        FlashSize = "256K";
        break;
    case 0x20:
        FlashSize = "1M";
        break;
    case 0x30:
        FlashSize = "2M";
        break;
    case 0x40:
        FlashSize = "4M";
        break;
    case 0x80:
        FlashSize = "8M";
        break;
    case 0x90:
        FlashSize = "16M";
        break;
    }

    const char *FlashMode = "";
    switch (flash_data[2] & 0xF)
    {
    case 0:
        FlashMode = "QIO";
        break;
    case 1:
        FlashMode = "QOUT";
        break;
    case 2:
        FlashMode = "DIO";
        break;
    case 3:
        FlashMode = "DOUT";
        break;
    }

    const char *FlashSpeed = "";
    switch (flash_data[3] & 0xF)
    {
    case 0x0:
        FlashSpeed = "40";
        break;
    case 0x1:
        FlashSpeed = "26";
        break;
    case 0x2:
        FlashSpeed = "20";
        break;
    case 0xF:
        FlashSpeed = "80";
        break;
    }

    snprintf(httpRespBuffer, sizeof(httpRespBuffer),
             "["
             "{\"id\":\"mac\", \"innerHTML\": \"%s\"},"
             "{\"id\":\"wifi_mode\", \"innerHTML\": \"%s\"},"
             "{\"id\":\"client_ip\", \"innerHTML\": \"%s\"},"
             "{\"id\":\"flash_mode\", \"innerHTML\": \"%s %s @ %sMHz\"},"
             "{\"id\":\"part_boot_conf\", \"innerHTML\": \"%s @ 0x%06x (%u bytes)\"},"
             "{\"id\":\"part_boot_act\", \"innerHTML\": \"%s @ 0x%06x (%u bytes)\"},"
             "{\"id\":\"part_idle\", \"innerHTML\": \"%s @ 0x%06x (%u bytes)\"},"
             "{\"id\":\"part_idle_content\", \"innerHTML\": \"%s\"},"
             "{\"id\":\"bootloader\", \"innerHTML\": \"%s\"},"
             "{\"id\":\"soc\", \"innerHTML\": \"%s Core %s v%s (%s)\"}"
             //  "{\"id\":\"chip\", \"innerHTML\": \"%s\"}"
             "%s"
             "]",
             (char *)sys_mac_str,
             wifi__mode,
             client_ipstr,
             FlashSize, FlashMode, FlashSpeed,
             sys_partConfigured->label, sys_partConfigured->address, sys_partConfigured->size,
             sys_partRunning->label, sys_partRunning->address, sys_partRunning->size,
             sys_partIdle->label, sys_partIdle->address, sys_partIdle->size,
             ((flash_erasedFactoryApp == true) ? "Third Party" : "Factory"),
             ((flash_erasedBootloader == true) ? "Arduino eboot (Third Party)" : "Espressif (Factory)"),
             (sys_chipInfo.cores == 1 ? "Single" : sys_chipInfoCores_s), sys_chipInfoModel_s, sys_chipInfoRev_s, sys_chipInfoFeatures_s,
             //  sys_chipInfoModel_s,
             (sysInfoTransmitted ? "" : ",{\"id\":\"debug-log\", \"innerHTML\": \"<pre>Ready to flash...</pre>\"}"));

    /** 
     * This URI is called on the first load of the index page, then again
     * if a flashing step fails. We only want to initialize the debug log
     * on the index page's initial load.
     * */
    if (!sysInfoTransmitted)
    {
        sysInfoTransmitted = true;
    }

    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Content-Type", "application/json");
    httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));

    return ESP_OK;
}

void server__stop(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

httpd_handle_t server__start(void)
{
    static const char *TAG = "server__start";

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_uri_handlers = 9;

    // Start the httpd server
    // httpd server is FreeRTOS Task with default priority of idle+5
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &flash);
        httpd_register_uri_handler(server, &undo);
        httpd_register_uri_handler(server, &backup);
        httpd_register_uri_handler(server, &status);
        httpd_register_uri_handler(server, &upload);
        httpd_register_uri_handler(server, &safeMode);
        httpd_register_uri_handler(server, &wifi);
        httpd_register_uri_handler(server, &wifiStatus);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

/***
 * 
 * HELPER FUNCTIONS
 * 
 * */

/*
  Sends messages to the frontend using by holding an HTTP response open and returning chunks.
  HTTP request object must be submitted to this task as a parameter.
*/
esp_err_t webui_msgPub(httpd_req_t *req, char *step, char *state, char *message)
{
    static const char *TAG = "webui_msgPub";

    if (safemode__enabled == false)
    {
        snprintf(httpRespBuffer, sizeof(httpRespBuffer),
                 "{"
                 "\"step\": \"%s\","
                 "\"state\": \"%s\","
                 "\"message\": \"%s\","
                 "\"type\": \"flashStatus\""
                 "}\n",
                 step, state, message);

        ESP_LOGI(TAG, "%s", httpRespBuffer);

        esp_err_t ret = httpd_resp_send_chunk(req, httpRespBuffer, -1);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Failed to send status message to webui.");
        }
    }

    return ESP_OK;
}

esp_err_t flashSuccessWrapUp(httpd_req_t *req)
{
    static const char *TAG = "flashSuccessWrapup";

    sprintf(httpRespBuffer,
            "Flashed %s successfully, rebooting...\n Sensitive operations occur on first boot and may take up to five minutes to complete.",
            user_url_buf);
    ESP_LOGI(TAG, "%s", httpRespBuffer);

    /** Report to Web UI [DONE] **/
    webui_msgPub(req, "6", FLASH_STATE_DONE, "Sensitive operations occur on first boot and may take up to five minutes to complete. It is absolutely critical that you do not cut power to the device during this time.");

    for (int i = 4; i >= 0; i--)
    {
        ESP_LOGI(TAG, "Restarting in %d seconds...", i);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Restarting....");

    /** Close HTTP Response **/
    httpd_resp_send_chunk(req, NULL, 0);

#if FEAT_DEBUG_LOG_TO_WEBUI
    /** Disable Log to Web **/
    esp_log_set_putchar(old_logger);
#endif

    return ESP_OK;
}

static int req_getClientIp(httpd_req_t *req, char ipstr[40])
{
    static const char *TAG = "req_getClientIp";

    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr;
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0)
    {
        ESP_LOGE(TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    // Convert to IPv4 string
    inet_ntop(AF_INET, &addr.sin6_addr.un.u32_addr[3], ipstr, 40);
    ESP_LOGI(TAG, "Client IP => %s", ipstr);

    return ESP_OK;
}