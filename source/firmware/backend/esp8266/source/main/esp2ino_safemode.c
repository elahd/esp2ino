#include "esp2ino_common.h"

static EventGroupHandle_t safemode__wifi_eventGroup;

httpd_handle_t safemode__server_start(void);
void safemode__server_stop(httpd_handle_t server);

static void safemode__wifi_eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t safemode__handleRoot(httpd_req_t *req);
static esp_err_t safemode__handleUpload(httpd_req_t *req);
static esp_err_t safemode__handleReboot(httpd_req_t *req);
static esp_err_t safemode__doFlash(httpd_req_t *req);

httpd_uri_t safemode__root = {.uri = "/", .method = HTTP_GET, .handler = safemode__handleRoot};
httpd_uri_t safemode__upload = {.uri = "/upload", .method = HTTP_PUT, .handler = safemode__handleUpload};
httpd_uri_t safemode__reboot = {.uri = "/undo", .method = HTTP_GET, .handler = handleUndo};
httpd_uri_t safemode__undo = {.uri = "/reboot", .method = HTTP_GET, .handler = safemode__handleReboot};

esp_err_t safemode__legacy_event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}

bool safemode__wifi_init(void)
{
    safemode__wifi_eventGroup = xEventGroupCreate();

    tcpip_adapter_init();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &safemode__wifi_eventHandler, NULL));

    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    tcpip_adapter_ip_info_t ap_ip_info;

    IP4_ADDR(&ap_ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ap_ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);

    tcpip_adapter_dhcp_status_t status;
    tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_AP, &status);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info);
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);

    wifi_config_t wifi_config_ap = {.ap = {.ssid = FALLBACK_SSID,
                                           .ssid_len = strlen(FALLBACK_SSID),
                                           .max_connection = FALLBACK_MAX_CON,
                                           .authmode = WIFI_AUTH_OPEN}};

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    vEventGroupDelete(safemode__wifi_eventGroup);

    return true;
}

static void safemode__wifi_eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    static const char *TAG = "wifi__eventHandler";

    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

static esp_err_t safemode__handleRoot(httpd_req_t *req)
{
    memset(httpRespBuffer, 0, sizeof httpRespBuffer);

    snprintf(httpRespBuffer, sizeof(httpRespBuffer),
             "<h3>esp2ino is in safe mode.</h3>"
             "Click <a href=\"/reboot\">here</a> to try booting in normal mode or check esp2ino's online documentation for instructions on flashing from safe mode.");

    httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));

    return ESP_OK;
}

static esp_err_t safemode__handleReboot(httpd_req_t *req)
{

    httpd_resp_send(req, NULL, 0);

    esp_restart();

    return ESP_OK;
}

static esp_err_t safemode__handleUpload(httpd_req_t *req)
{
    // static const char *TAG = "server__handleUpload";

    memset(httpRespBuffer, 0, sizeof httpRespBuffer);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_set_status(req, HTTPD_200);

    if (safemode__doFlash(req) == ESP_OK)
    {
        flashSuccessWrapUp(req);
        snprintf(httpRespBuffer, sizeof(httpRespBuffer),
                 "<strong>Flash successful. Rebooting.</strong> Sensitive operations occur on first boot and may take up to five minutes to complete. It is absolutely critical that you do not cut power to the device during this time.");
        httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));
        esp_restart();
    }
    else
    {
        snprintf(httpRespBuffer, sizeof(httpRespBuffer),
                 "<strong>Flash failed.</strong>");
        httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));
    }

    return ESP_OK;
}

static esp_err_t safemode__doFlash(httpd_req_t *req)
{
    static const char *TAG = "ota_task";
    esp_err_t ret;

    /***
     * DOWNLOAD & FLASH APP
     * */

    ESP_LOGI(TAG, "BEGIN: Downloading and flashing third party bin.");

    ret = flash_viaUpload(req);

    if (ret != ESP_OK)
        return ret;

    ESP_LOGI(TAG, "DONE: Downloading and flashing third party bin.");

    /***
     * ERASE FLASH - BOOTLOADER
     *
     * Point of No Return. If there's an error here, we're screwed.
     * */

    ESP_LOGI(TAG, "BEGIN: Erasing bootloader.");

    // Failing here will PROBABLY brick the device.
    ret = spi_flash_erase_range(0x0, 0x10000);

#if DEV_FORCE_FAIL
    ret = ESP_FAIL;
#endif

    flash_erasedBootloader = true;

    if (ret != ESP_OK)
    {
        flash_erasedBootloader = true;

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DONE: Erasing bootloader.");

    /***
   * WRITE BOOTLOADER
   * */

    ESP_LOGI(TAG, "BEGIN: Writing bootloader.");

    unsigned char *eboot_bin_ptr = eboot_bin;

    // Preserve existing flash settings in bootloader
    eboot_bin_ptr[2] = flash_data[2];
    eboot_bin_ptr[3] = flash_data[3];

    ret = spi_flash_write(0x0, (uint32_t *)eboot_bin_ptr, eboot_bin_len);

    if (ret != ESP_OK)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DONE: Writing bootloader.");

    ESP_LOGI(TAG, "BEGIN: Setting eboot commands.");

    // Command the bootloader to copy the firmware to the correct place on next
    // boot
    struct eboot_command ebcmd;
    ebcmd.action = ACTION_COPY_RAW;
    ebcmd.args[0] = sys_partIdle->address;
    ebcmd.args[1] = 0x0;
    ebcmd.args[2] = sys_partIdle->size;

    eboot_command_write(&ebcmd);

    struct eboot_command ebcmd_read;
    eboot_command_read(&ebcmd_read);
    ESP_LOGI(TAG, "Action:          %i", ebcmd_read.action);
    ESP_LOGI(TAG, "Source Addr:     0x%x", ebcmd_read.args[0]);
    ESP_LOGI(TAG, "Dest Addr:       0x%x", ebcmd_read.args[1]);
    ESP_LOGI(TAG, "Size:            0x%x", ebcmd_read.args[2]);

    ESP_LOGI(TAG, "DONE: Setting eboot commands.");

    /***
    * ERASE SYSTEM PARAMS FROM FLASH
    * */

    ESP_LOGI(TAG, "BEGIN: Erasing RF calibration data.");

    for (uint16_t i = 244; i < 256; i++)
    {
        ESP_LOGI(TAG, "Erasing sector %d", i);
        if (spi_flash_erase_sector(i) != ESP_OK)
            ESP_LOGW(TAG, "Failed to erase sector.");
    }

    ESP_LOGI(TAG, "DONE: Erasing RF calibration data.");

    return ESP_OK;
}

void safemode__server_stop(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

httpd_handle_t safemode__server_start(void)
{
    static const char *TAG = "safemode__server_start";

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    // httpd server is FreeRTOS Task with default priority of idle+5
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &safemode__root);
        httpd_register_uri_handler(server, &safemode__upload);
        httpd_register_uri_handler(server, &safemode__undo);
        httpd_register_uri_handler(server, &safemode__reboot);

        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}