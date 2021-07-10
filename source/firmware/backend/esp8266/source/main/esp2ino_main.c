#include "esp2ino_common.h"

/**
 * TO-DO
 * 1. uxTaskGetStackHighWaterMark() to tune task stack size for http server.
 * 2. Have webui pop up automatically after connecting using captive portal protocol.
 * 3. When connecting to Wi-Fi with bad credentials, webui times out before esp2ino finishes trying to connect. If you requeest another wifi scan while esp2ino is still trying to connect, the scan will silently fail.
 */

/**
 * DECLARE LOCALS
 * */

static char flashTsk_logBuffer[4096];
static httpd_handle_t server;

/**
 * INITIALIZE GLOBALS
 * */

/** System Attributes **/
char sys_hostname[25] = "esp2ino";
ip4_addr_t wifi__ip_ap;
char wifi__staSsid[32];
char wifi__staBssid[6];
char wifi__ip_sta_str[INET_ADDRSTRLEN];
char wifi__ip_ap_str[INET_ADDRSTRLEN];
char *wifi__mode;
bool wifi__isConnected;
char wifi__channel[6];
esp_chip_info_t sys_chipInfo;
char sys_chipInfoFeatures_s[50];
char *sys_chipInfoModel_s;
char sys_chipInfoCores_s[8];
char sys_chipInfoRev_s[8];
const esp_partition_t *sys_partRunning;
const esp_partition_t *sys_partConfigured;
const esp_partition_t *sys_partIdle;
char sys_hostname[25];
SemaphoreHandle_t wifi__connectSemaphore;
unsigned int sys_realFlashSize;
uint8_t sys_mac[6];
char sys_mac_str[18];
char sys_statusMsg[100];
char user_url_buf[200];
char httpRespBuffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4)));
esp_err_t flashSuccessWrapUp(httpd_req_t *req);
uint8_t flash_data[4];

/** Reboot Agent **/
TaskHandle_t xRebootAgent = NULL;
EventGroupHandle_t rebootAgent_EventGroup;

/** Flashing **/
bool flash_erasedBootloader = false;
bool flash_erasedFactoryApp = false;

/**
 * DECLARE STATIC FUNCTIONS
 * */

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt);
static void init_mdns(void);
static void sys_setMacAndHost(int wifiMode);
static esp_err_t legacy_event_handler(void *ctx, system_event_t *event);
static esp_err_t flash_viaDownload(httpd_req_t *req);
static esp_err_t vCreateRebootAgent(void);
static void rebootAgentTask(void *pvParameters);
static bool sys_safeModeCheck(void);

static const char *TAG = "global";

/**
 * 
 * MDNS FUNCTIONS
 * 
 */

#if CONFIG_IDF_TARGET_ESP8266
static esp_err_t legacy_event_handler(void *ctx, system_event_t *event)
{
    mdns_handle_system_event(ctx, event);
    return ESP_OK;
}
#endif

static void init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err)
    {
        printf("MDNS Init failed: %d\n", err);
        return;
    }

    ESP_ERROR_CHECK(mdns_instance_name_set("espino_mdns"));

    mdns_hostname_set(sys_hostname);
    mdns_instance_name_set("esp2ino");

    mdns_service_add(NULL, "_esp2ino", "_tcp", 80, NULL, 0);

    mdns_txt_item_t serviceTxtData[4] = {
        {"chip_model", sys_chipInfoModel_s},
        {"chip_rev", sys_chipInfoRev_s},
        {"chip_cores", sys_chipInfoCores_s},
        {"chip_features", sys_chipInfoFeatures_s}};

    mdns_service_txt_set("_esp2ino", "_tcp", serviceTxtData, 4);
}

/***
 * 
 * 
 * CORE FUNCTIONS
 * 
 * 
 * */

/***
 * Reboots esp2ino after flash functions.
 * */
static esp_err_t vCreateRebootAgent(void)
{
    static const char *TAG = "vCreateRebootAgent";
    BaseType_t xReturned;

    rebootAgent_EventGroup = xEventGroupCreate();

    /* Create the task, storing the handle. */
    xReturned = xTaskCreate(
        rebootAgentTask,  /* Function that implements the task. */
        "rebootAgent",    /* Text name for the task. */
        1024,             /* Stack size in words, not bytes. */
        NULL,             /* Parameter passed into the task. */
        tskIDLE_PRIORITY, /* Priority at which the task is created. */
        &xRebootAgent);   /* Used to pass out the created task's handle. */

    if (xReturned != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create reboot agent.");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void rebootAgentTask(void *pvParameters)
{
    static const char *TAG = "rebootAgentTask";

    for (;;)
    {
        EventBits_t bits = xEventGroupWaitBits(rebootAgent_EventGroup,
                                               REBOOT_NOW_BIT,
                                               pdTRUE,
                                               pdFALSE,
                                               portMAX_DELAY);

        if (bits && REBOOT_NOW_BIT)
        {
            ESP_LOGI(TAG, "Reboot request received.");

            vTaskDelay(5000 / portTICK_PERIOD_MS);

            // Stop server
            server__stop(server);

            // for (int i = 4; i >= 0; i--)
            // {
            //     ESP_LOGI(TAG, "Restarting in %d seconds...", i);

            //     vTaskDelay(1000 / portTICK_PERIOD_MS);
            // }

            ESP_LOGI(TAG, "Restarting....");

            esp_restart();
        }
    }
}

/***
 * 
 * FLASHING FUNCTIONS
 *
 * */

/**
 * Event handler for HTTP CLIENT. Used by OTA handler when fetching firmware. 
 * ENHANCEMENT: Use this to show download progress for firmware.
 *              Also to confirm that image will fit on partition.
 */

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt)
{
    static const char *TAG = "http_event";

    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
                 evt->header_value);
        // if (strcasecmp(evt->header_key, "Content-Length") == 0)
        // {
        //     ESP_LOGI(TAG, "%s: %s", evt->header_key, evt->header_value);
        // }
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

esp_err_t flash_viaUpload(httpd_req_t *req)
{
    static const char *TAG = "flash_viaUpload";

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    ESP_LOGI(TAG, "Starting OTA via Upload.");
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL)
    {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        return err;
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");

    esp_err_t ota_write_err = ESP_OK;
    char *upgrade_data_buf = (char *)malloc(OTA_UPLD_BUF_SIZE);
    if (!upgrade_data_buf)
    {
        ESP_LOGE(TAG, "Couldn't allocate memory to upgrade data buffer");
        return ESP_ERR_NO_MEM;
    }

    int ret;
    int ota_total = req->content_len;
    int remaining = req->content_len;
    int last_logged = req->content_len;
    while (1)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, upgrade_data_buf, MIN(remaining, sizeof(upgrade_data_buf)))) < 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                ESP_LOGW(TAG, "HTTP socket timeout.");
                /* Retry receiving if timeout occurred */
                continue;
            }
            ESP_LOGI(TAG, "Unknown http read error.");
            return ESP_FAIL;
        }
        if (ret == 0)
        {
            ESP_LOGI(TAG, "Connection closed,all data received");
            break;
        }
        if (ret < 0)
        {
            ESP_LOGE(TAG, "Error: SSL data read error");
            break;
        }
        if (ret > 0)
        {
            ota_write_err = esp_ota_write(update_handle, (const void *)upgrade_data_buf, ret);
            if (ota_write_err != ESP_OK)
            {
                ESP_LOGE(TAG, "Flashing failed with error %i", ota_write_err);
                break;
            }
            remaining -= ret;

            if ((last_logged - remaining) > 10000)
            {
                ESP_LOGI(TAG, "%i", remaining);
                last_logged = remaining;
            }
        }
    }
    free(upgrade_data_buf);

    ESP_LOGD(TAG, "Total binary data length writen: %d", ota_total);

    esp_err_t ota_end_err = esp_ota_end(update_handle);
    if (ota_write_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%d", err);
        return ota_write_err;
    }
    else if (ota_end_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error: esp_ota_end failed! err=0x%d. Image is invalid", ota_end_err);
        return ota_end_err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%d", err);
        return err;
    }
    ESP_LOGI(TAG, "esp_ota_set_boot_partition succeeded");

    return ESP_OK;
}

static esp_err_t flash_viaDownload(httpd_req_t *req)
{
    static const char *TAG = "flash_viaDownload";
    esp_err_t ret;

    ESP_LOGI(TAG, "Starting OTA via Download.");

    esp_http_client_config_t config = {.url = user_url_buf,
                                       .event_handler = ota_http_event_handler,
                                       .disable_auto_redirect = false,
                                       .skip_cert_common_name_check = true};

    int attempt = 0;
    while (attempt <= 4)
    {

#if DEV_FAKE_WRITE
        ret = ESP_OK;
#else
        ret = esp_https_ota(&config);
#endif

        if (ret == ESP_ERR_HTTP_CONNECT && attempt < 4)
        {
            ESP_LOGW(TAG, "HTTP connection failed. Trying again...");

            /** Report to Web UI [Task 1 of 5] **/
            snprintf(flashTsk_logBuffer, sizeof(flashTsk_logBuffer), "Failed to download firmware. Trying again. Attempt %d of 5.", attempt + 1);
            webui_msgPub(req, "1", FLASH_STATE_RETRY, flashTsk_logBuffer);

            printf("Retrying in:\n");
            for (int i = 4; i >= 0; --i)
            {
                printf("%d ", i + 1);
                fflush(stdout);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            printf("\n");
        }
        else if (ret == ESP_OK)
        {

            return ESP_OK;
        }
        else
        {
            break;
        }

        ++attempt;
    }

    return ESP_FAIL;
}

esp_err_t doFlash(httpd_req_t *req, int otaType)
{
    static const char *TAG = "ota_task";
    esp_err_t ret;

    /***
   * INITIALIZE / PRE-CHECK
   * */

    // ESP_LOGI(TAG, "Running from %s at 0x%x.", sys_partRunning->label,
    //          sys_partRunning->address);

    /***
     * DOWNLOAD & FLASH APP
     * */

    ESP_LOGI(TAG, "BEGIN: Downloading and flashing third party bin.");

    /** Report to Web UI [Task 1 of 5] **/
    webui_msgPub(req, "1", FLASH_STATE_RUNNING, "");

    if (otaType == OTA_TYPE_DOWNLOAD)
    {
        ret = flash_viaDownload(req);
    }
    else if (otaType == OTA_TYPE_UPLOAD)
    {
        ret = flash_viaUpload(req);
    }
    else
    {
        /** Report to Web UI [Task 1 of 5] **/
        webui_msgPub(req, "1", FLASH_STATE_FAIL, "Unable to determine OTA method.");
        return ESP_FAIL;
    }

    if (ret == ESP_OK)
    {
        flash_erasedFactoryApp = true;

        /** Report to Web UI [Task 1 of 5] **/
        webui_msgPub(req, "1", FLASH_STATE_SUCCESS, "");
    }
    else
    {
        ESP_LOGE(TAG, "Flash failed with error 0x%x", ret);
        /** Report to Web UI [Task 1 of 5] **/
        webui_msgPub(req, "1", FLASH_STATE_FAIL, "Failed while installing firmware. This should be recoverable. Try again after verifying that your "
                                                 "internet connection is working and that your firmware URL is correct. "
                                                 "<br><br>⚠️ DO NOT REVERT TO FACTORY FIRMWARE! ⚠️<br><br>"
                                                 "Factory firmware may have been overwritten during this flashing attempt. "
                                                 "Reverting to this overwritten firmware will brick your device. You can reboot / power cycle your device if needed to return to esp2ino.");

        flash_erasedFactoryApp = true;
        return ret;
    }

    ESP_LOGI(TAG, "DONE: Downloading and flashing third party bin.");

    /***
     * ERASE FLASH - BOOTLOADER
     *
     * Point of No Return. If there's an error here, we're screwed.
     * */

    ESP_LOGI(TAG, "BEGIN: Erasing bootloader.");

    /** Report to Web UI [Task 2 of 5] **/
    webui_msgPub(req, "2", FLASH_STATE_RUNNING, "");

    // Failing here will PROBABLY brick the device.
    ret = spi_flash_erase_range(0x0, 0x10000);

#if DEV_FORCE_FAIL
    ret = ESP_FAIL;
#endif

    flash_erasedBootloader = true;

    if (ret != ESP_OK)
    {
        flash_erasedBootloader = true;

        /** Report to Web UI [Task 2 of 5] **/
        webui_msgPub(req, "2", FLASH_STATE_FAIL, "Failed while erasing bootloader. Your device is probably bricked 😬. Sorry!");

        // ESP_LOGD(TAG, "%s", (char *)tmRet);
        /** END Report to Web UI **/

        return ESP_FAIL;
    }

    /** Report to Web UI [Task 2 of 5] **/
    webui_msgPub(req, "2", FLASH_STATE_SUCCESS, "");

    ESP_LOGI(TAG, "DONE: Erasing bootloader.");

    /***
   * WRITE BOOTLOADER
   * */

    ESP_LOGI(TAG, "BEGIN: Writing bootloader.");

    /** Report to Web UI [Task 3 of 5] **/
    webui_msgPub(req, "3", FLASH_STATE_RUNNING, "");

    unsigned char *eboot_bin_ptr = eboot_bin;

    // Preserve existing flash settings in bootloader
    eboot_bin_ptr[2] = flash_data[2];
    eboot_bin_ptr[3] = flash_data[3];

    ret = spi_flash_write(0x0, (uint32_t *)eboot_bin_ptr, eboot_bin_len);

    if (ret != ESP_OK)
    {

        /** Report to Web UI [Task 3 of 5] **/
        webui_msgPub(req, "3", FLASH_STATE_FAIL, "Failed while writing Arduino bootloader. Your device is probably bricked 😬. Sorry!");

        return ESP_FAIL;
    }

    /** Report to Web UI [Task 3 of 5] **/
    webui_msgPub(req, "3", FLASH_STATE_SUCCESS, "");

    ESP_LOGI(TAG, "DONE: Writing bootloader.");

    ESP_LOGI(TAG, "BEGIN: Setting eboot commands.");

    /** Report to Web UI [Task 4 of 5] **/
    webui_msgPub(req, "4", FLASH_STATE_RUNNING, "");

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

    /** Report to Web UI [Task4  of 5] **/
    webui_msgPub(req, "4", FLASH_STATE_SUCCESS, "");

    /***
    * ERASE SYSTEM PARAMS FROM FLASH
    * 
    * Clear flash in area used by Arduino SDK for system parameters.
    * Helps avoid strange behavior in third party firmware.
    * 
    * Copied from https://github.com/vtrust-de/esp8266-ota-flash-convert.
    * system params [253,256), rf data [251,253), rtos params [247,256), tasmota settings [244,253)
    * */

    /** Report to Web UI [Task 5 of 5] **/
    webui_msgPub(req, "5", FLASH_STATE_RUNNING, "");

    ESP_LOGI(TAG, "BEGIN: Erasing RF calibration data.");

    for (uint16_t i = 244; i < 256; i++)
    {
        ESP_LOGI(TAG, "Erasing sector %d", i);
        if (spi_flash_erase_sector(i) != ESP_OK)
            ESP_LOGW(TAG, "Failed to erase sector.");
    }

    ESP_LOGI(TAG, "DONE: Erasing RF calibration data.");

    /** Report to Web UI [Task 5 of 5] **/
    webui_msgPub(req, "5", FLASH_STATE_SUCCESS, "");

    return ESP_OK;
}

/**
 * 
 * HELPER FUNCTIONS
 * 
 */

static void sys_setMacAndHost(int wifi__mode)
{
    esp_mac_type_t macType = (wifi__mode == WIFI_MODE_STA) ? ESP_MAC_WIFI_STA : ESP_MAC_WIFI_SOFTAP;

    esp_read_mac(sys_mac, macType);

    snprintf(sys_mac_str, sizeof(sys_mac_str), MACSTR, sys_mac[0],
             sys_mac[1], sys_mac[2], sys_mac[3], sys_mac[4], sys_mac[5]);

    ESP_LOGI(TAG, "MAC Address: %s", sys_mac_str);

#if FEAT_MAC_IN_MDNS_HOSTNAME
    // Generate sys_hostname
    snprintf(sys_hostname, sizeof(sys_hostname), "esp2ino-%02X%02X%02X", sys_mac[3], sys_mac[4], sys_mac[5]);
    ESP_LOGI(TAG, "mdns sys_hostname: %s", sys_hostname);
#else
    snprintf(sys_hostname, sizeof(sys_hostname), "esp2ino");
    ESP_LOGI(TAG, "mdns sys_hostname: %s", sys_hostname);
#endif
}

/**
 * 
 * ESP2INO INITIALIZATION FUNCTIONS
 * 
 */

static void esp2ino_init(void)
{
#if DEV_FORCE_SAFEMODE
    ESP_LOGW(TAG, "TRIGGERING SAFE MODE. (Development Toggle)");
    ESP_ERROR_CHECK(ESP_FAIL);
#endif

    safemode__enabled = false;

    wifi__connectSemaphore = xSemaphoreCreateMutex();

#if CONFIG_IDF_TARGET_ESP8266
    // Register mDNS with the legacy event loop as per https://github.com/espressif/ESP8266_RTOS_SDK/issues/870.
    esp_event_loop_init(legacy_event_handler, NULL);
#endif

    // Retrieve flash headers
    ESP_LOGI(TAG, "BEGIN: Retrieving system data.");

    sys_partRunning = esp_ota_get_running_partition();
    sys_partConfigured = esp_ota_get_boot_partition();
    sys_partIdle = esp_ota_get_next_update_partition(NULL);

    if (sys_partRunning->address != sys_partConfigured->address)
    {
        // We may want to take action here. For now, just report.
        ESP_LOGW(TAG,
                 "Partition %s at 0x%x configured as boot partition, but currently "
                 "running from %s at 0x%x.",
                 sys_partConfigured->label, sys_partConfigured->address,
                 sys_partRunning->label, sys_partRunning->address);
    }

    spi_flash_read(0x0, (uint32_t *)flash_data, 4);
    ESP_LOGI(TAG,
             "Magic: %02x, Segments: %02x, Flash Mode: %02x, Size/Freq: %02x",
             flash_data[0], flash_data[1], flash_data[2], flash_data[3]);

    // ENHANCEMENT: Use esp_partition_find to confirm assumptions about flash layout and to get actual chip size.
    sys_realFlashSize = spi_flash_get_chip_size();

    esp_chip_info(&sys_chipInfo);

    snprintf(sys_chipInfoCores_s, sizeof(sys_chipInfoCores_s), "%d", sys_chipInfo.cores);
    snprintf(sys_chipInfoRev_s, sizeof(sys_chipInfoCores_s), "%d", sys_chipInfo.revision);

    sys_chipInfoModel_s = "ESP8266";

    ESP_LOGI(TAG, "%s", sys_chipInfoModel_s);

    snprintf(sys_chipInfoFeatures_s, sizeof(sys_chipInfoFeatures_s), "%s%s%s%s",
             sys_chipInfo.features & CHIP_FEATURE_WIFI_BGN ? "802.11bgn " : "",
             sys_chipInfo.features & CHIP_FEATURE_BLE ? "BLE " : "",
             sys_chipInfo.features & CHIP_FEATURE_BT ? "BT " : "",
             sys_chipInfo.features & CHIP_FEATURE_EMB_FLASH ? "Embedded-Flash" : "External-Flash");

    // This gives us a MAC address and hostname to use immediately.
    // Will be called again if we enter fallback mode. (AP/STA modes have different MACs).
    sys_setMacAndHost(WIFI_MODE_AP);

    init_mdns();

    ESP_LOGI(TAG, "DONE: Retrieving system data.");

    ESP_LOGI(TAG, "BEGIN: Start AP and web server.");

    ESP_LOGI(TAG, "Starting Wi-Fi.");

    /* TO-DO: If this fails, force safe mode --- or boot in AP-only mode. */
    wifi__apSta_init();

    ESP_LOGI(TAG, "BEGIN: Start system reboot agent.");
    vCreateRebootAgent();
    ESP_LOGI(TAG, "BEGIN: Start system reboot agent.");

    if (server == NULL)
    {
        ESP_LOGI(TAG, "Starting webserver");
        server = server__start();
    }

    ESP_LOGI(TAG, "DONE: Start AP and web server.");

    ESP_LOGI(TAG, "Initialization complete. Waiting for user command.");

    strcpy(sys_statusMsg, "Ready to flash.");
}

static void esp2ino_init_safe(void)
{
    safemode__enabled = true;

#if CONFIG_IDF_TARGET_ESP8266
    // IDF throws an error without this, even though mDNS disabled in safe mode.
    esp_event_loop_init(safemode__legacy_event_handler, NULL);
#endif

    // Retrieve flash headers
    ESP_LOGI(TAG, "BEGIN: Retrieving system data.");

    sys_partRunning = esp_ota_get_running_partition();
    sys_partConfigured = esp_ota_get_boot_partition();
    sys_partIdle = esp_ota_get_next_update_partition(NULL);

    spi_flash_read(0x0, (uint32_t *)flash_data, 4);
    ESP_LOGI(TAG,
             "Magic: %02x, Segments: %02x, Flash Mode: %02x, Size/Freq: %02x",
             flash_data[0], flash_data[1], flash_data[2], flash_data[3]);

    ESP_LOGI(TAG, "DONE: Retrieving system data.");

    ESP_LOGI(TAG, "BEGIN: Start AP and web server.");

    safemode__wifi_init();

    if (server == NULL)
    {
        server = safemode__server_start();
    }

    ESP_LOGI(TAG, "DONE: Start AP and web server.");

    ESP_LOGI(TAG, "Initialization complete. Waiting for user command.");
}

/**
 * 
 * ESP2INO ENTRY FUNCTIONS
 * 
 */

static bool sys_safeModeCheck(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason_early();

    if (reset_reason == ESP_RST_PANIC || reset_reason == ESP_RST_TASK_WDT || reset_reason == ESP_RST_WDT || reset_reason == ESP_RST_INT_WDT || reset_reason == ESP_RST_UNKNOWN)
    {
        ESP_LOGW(TAG, "SAFE MODE TRIGGERED. (Reset Reason %i)", reset_reason);
        return true;
    }
    else
    {
        ESP_LOGW(TAG, "No need for safe mode. (Reset Reason %i)", reset_reason);
        return false;
    }
}

extern void app_main()
{
    esp_log_level_set("esp_timer", ESP_LOG_INFO);

    uart_set_baudrate(0, 115200); // Use 115200 for compatibility with gdb.

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(nvs_flash_init());

    IP4_ADDR(&wifi__ip_ap, 10, 0, 0, 1);

    // Start ESP-IDF-style event loop.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    sys_safeModeCheck() ? esp2ino_init_safe() : esp2ino_init();
}