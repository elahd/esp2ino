#include "esp2ino_main.h"
#include "eboot_command.h"

#include "index.html.gz.h"

#if CONFIG_IDF_TARGET_ESP32
// #include "eboot_esp32_bin.h"
#elif CONFIG_IDF_TARGET_ESP8266
#include "eboot_bin.h"
#endif

/**
 * Flags & Options
 * */

/** DEVELOPMENT FLAGS **/    /** These must all be false before publishing release! **/
#define DEV_FAKE_WRITE false // Skips actual OTA task. Defaults to success.
#define DEV_FORCE_FAIL false // Forces a failure on bootloader erase step.

/** FEATURE TOGGLES **/
#define FEAT_DEBUG_LOG_TO_WEBUI false   // See note before webui_debugPub definition, below.
#define FEAT_MODERN_WIFI_ONLY false     // Only connect to WPA2 networks.
#define FEAT_MAC_IN_MDNS_HOSTNAME false // Include MAC address in mDNS hostname (esp2ino-fa348d vs esp2ino)

/**
 * Pre-Processor Directives & Variables
 * */

/** Used to signal events to Web UI. **/
#define FLASH_STATE_INACTIVE "0"
#define FLASH_STATE_RUNNING "1"
#define FLASH_STATE_SUCCESS "2"
#define FLASH_STATE_FAIL "3"
#define FLASH_STATE_RETRY "4"
#define FLASH_STATE_WARN "5"
#define FLASH_STATE_DONE "6"

/** Used for aligning httpRespBuffer variables **/
#define SPI_FLASH_SEC_SIZE 4096

/** Wi-Fi **/
#define STA_WIFI_CONNECTED_BIT BIT0
#define STA_WIFI_FAIL_BIT BIT1
#define STA_WIFI_NUM_ATTEMPTS 5
#define SMARTCOFNIG_TYPE SC_TYPE_ESPTOUCH
#define FALLBACK_SSID "esp2ino"
#define FALLBACK_MAX_CON 10

/** Misc **/
#define BACKUP_BUF_SIZE 250
#define LOG_BUF_MAX_LINE_SIZE 250
#define OTA_UPLD_BUF_SIZE 250

/**
 * GLOBALS
 * */

/** Flashing **/
uint32_t flash_data;
uint8_t *flash_dataAddr = (uint8_t *)&flash_data;

bool flash_erasedBootloader = false;
bool flash_erasedFactoryApp = false;

char user_url_buf[200];

int otaDl_Pct = 0;
int otaDl_Size = 0;
int otaDl_SoFar = 0;

/** System Attributes **/
static unsigned int sys_realFlashSize;
uint8_t sys_mac[6];
char sys_mac_str[18];
char sys_statusMsg[100];
char sys_ip[32];
const esp_partition_t *sys_partRunning;
const esp_partition_t *sys_partConfigured;
const esp_partition_t *sys_partIdle;
bool sysInfoTransmitted = false;
esp_chip_info_t sys_chipInfo;
char sys_chipInfoFeatures_s[50];
char *sys_chipInfoModel_s;
char sys_chipInfoCores_s[8];
char sys_chipInfoRev_s[8];
char sys_hostname[25] = "esp2ino";

/** Logging **/
#if FEAT_DEBUG_LOG_TO_WEBUI
static putchar_like_t old_logger = NULL;
#endif
char debug_logBuffer[LOG_BUF_MAX_LINE_SIZE];
char flashTsk_logBuffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4))) = {0};
int webMsgQueueSize = 0;

/** General Connectivity **/
char httpRespBuffer[SPI_FLASH_SEC_SIZE * 4] __attribute__((aligned(4))) = {0};
static httpd_handle_t server = NULL;
httpd_req_t *flash_httpdReq;
// wifi_config_t saved_wifi_config;
char *wifiMode = "Unknown";
static const wifi_config_t emptyStruct;

/** Stored / Sta WiFi Mode **/
static EventGroupHandle_t staWifi_EventGroup;
static int staWifi_retryNum;

/** SmartConfig **/
TimerHandle_t smartConfig_fail;
bool smartConfig_started = false;
static EventGroupHandle_t smartConfig_EventGroup;
uint8_t smartConfig_ssid[250] = {0};
uint8_t smartConfig_password[250] = {0};
uint8_t smartConfig_rcvdData[250] = {0};

static const int SMARTCONFIG_CONNECTED_BIT = BIT0;
static const int SMARTCONFIG_DONE_BIT = BIT1;

static const char *TAG = "global";

/***
 * 
 * FUNCTIONS
 * 
 * */

/***
 * WIFI WORKFLOW
 * 
 * 1) [STORED CONFIG] esp2ino first tried to use wifi credentials saved by the factory firmware.
 *    After 5 failed attempts, firmware starts the smartconfig workflow. esp2ino can check for
 *    credentials saved using either Espressif's Wi-Fi library or Wyze firmware.
 * 
 * 2) [SMARTCONFIG] esp2ino first tries to establish a connection using espressif esptouch / smartconfig.
 *    If a connection can't be made within 2 minutes, the firmware will enter "fallback mode."
 * 
 * 3) [FALLBACK MODE] The firmware will create a WiFi network named esp2ino. Clients can connect to this
 *    network to install firmware from a server on their local computer.
 * 
 * 4) [FALLBACK TIMER] The smartconfig event handler resets the 2 minute fallback timer several times
 *    while trying to connect. This prevents us from falling back when Wi-Fi is available, but slow to
 *    connect.
 * */

/**
 * "STORED CONFIG" / STATION MODE
 */
static void staWifi_eventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{

    if (!staWifi_retryNum)
        staWifi_retryNum = 0;

    static const char *TAG = "wifi event handler";

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (staWifi_retryNum < STA_WIFI_NUM_ATTEMPTS)
        {
            esp_wifi_connect();
            staWifi_retryNum++;
            ESP_LOGI(TAG, "Not connected to AP. Retrying...");
        }
        else
        {
            xEventGroupSetBits(staWifi_EventGroup, STA_WIFI_FAIL_BIT);
            ESP_LOGI(TAG, "Not connected to AP. Giving up.");
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:%s",
                 ip4addr_ntoa(&event->ip_info.ip));
        staWifi_retryNum = 0;
        xEventGroupSetBits(staWifi_EventGroup, STA_WIFI_CONNECTED_BIT);
    }
}

/**
 * Loads Wi-Fi credentials stored by Wyze firmware.
 * wifi_config must be 0 initialized.
 */
bool staWifiCreds_getWyze(wifi_config_t *wifi_config)
{
    static const char *TAG = "staWifiCreds_getWyze";

    ESP_LOGI(TAG, "BEGIN: Searching for Wi-Fi credendials saved by Wyze firmware.");

    bool ret;
    size_t wyzeSsid_size;
    size_t wyzePass_size;
    char wyzeSsid[32];
    char wyzePass[64];

    while (1)
    {
        nvs_handle nvs_cred_handle;
        if (nvs_open("user_wifi", NVS_READWRITE, &nvs_cred_handle) != ESP_OK)
        {
            ret = false;
            break;
        }

        // SSID
        if (nvs_get_str(nvs_cred_handle, "ssid", NULL, &wyzeSsid_size) != ESP_OK)
        {
            ret = false;
            break;
        }

        if (nvs_get_str(nvs_cred_handle, "ssid", wyzeSsid, &wyzeSsid_size) != ESP_OK)
        {
            ret = false;
            break;
        }

        if (*wyzeSsid == '\0')
        {
            ret = false;
            break;
        }

        ESP_LOGI(TAG, "Found SSID: %s", wyzeSsid);

        // Password
        if (nvs_get_str(nvs_cred_handle, "pass", NULL, &wyzePass_size) != ESP_OK)
        {
            ret = false;
            break;
        }

        if (nvs_get_str(nvs_cred_handle, "pass", wyzePass, &wyzePass_size) != ESP_OK)
        {
            ret = false;
            break;
        }

        ESP_LOGI(TAG, "Found Password: %s", wyzePass);

        ret = true;

        break;
    }

    if (ret == true)
    {
        memcpy(wifi_config->sta.ssid, wyzeSsid, wyzeSsid_size);
        memcpy(wifi_config->sta.password, wyzePass, wyzePass_size);
    }
    else
    {
        ESP_LOGI(TAG, "Failed to load Wyze creds.");
    }

    ESP_LOGI(TAG, "END: Searching for Wi-Fi credendials saved by Wyze firmware.");

    return ret;
}

/**
 * Loads Wi-Fi credentials stored by Espressif's Wi-Fi library.
 */
bool staWifiCreds_getEsp(wifi_config_t *wifi_config)
{
    static const char *TAG = "staWifiCreds_getEsp";
    ESP_LOGI(TAG, "BEGIN: Searching for Wi-Fi credendials saved by Espressif's Wi-Fi libraries.");

    bool ret;
    uint8_t empty_bssid[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    while (1)
    {
        if (esp_wifi_get_config(ESP_IF_WIFI_STA, wifi_config) != ESP_OK)
        {
            ESP_LOGI(TAG, "Failed to load stored Wi-Fi config from NVS.");
            ret = false;
            break;
        }

        if (wifi_config->sta.ssid[0] == '\0')
        {
            ret = false;
            break;
        }

        ESP_LOGI(TAG, "Found Stored Config (SSID: %s, BSSID: " MACSTR ", Password: %s)\n\r", wifi_config->sta.ssid, MAC2STR(wifi_config->sta.bssid), wifi_config->sta.password);

        if (strlen((const char *)wifi_config->sta.ssid) == 0 && memcmp(empty_bssid, wifi_config->sta.bssid, sizeof(empty_bssid)) == 0)
        {
            ESP_LOGI(TAG, "Invalid stored Wi-Fi credentials.");
            ret = false;
            break;
        }

        ret = true;

        break;
    }

    if (ret == false)
    {
        ESP_LOGI(TAG, "Failed to load Espressif creds.");
        memcpy(wifi_config, &emptyStruct, sizeof(emptyStruct));
    }

    ESP_LOGI(TAG, "END: Searching for Wi-Fi credendials saved by Espressif's Wi-Fi libraries.");

    return ret;
}

static bool staWifi_init(void)
{
    // Don't use ESP_ERROR_CHECK in here. If there's a Wi-Fi connection failure,
    // we should exit this function gracefully instead of aborting.

    static const char *TAG = "staWifi_init";

    staWifi_EventGroup = xEventGroupCreate();

    tcpip_adapter_init();

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* Load Stored WiFi Config */
    wifi_config_t wifi_config;
    if (staWifiCreds_getEsp(&wifi_config))
    {
        wifiMode = "Connected to Wi-Fi via Saved Credentials (Espressif)";
    }
    else if (staWifiCreds_getWyze(&wifi_config))
    {
        wifiMode = "Connected to Wi-Fi via Saved Credentials (Wyze)";
    }
    else
    {
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &staWifi_eventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &staWifi_eventHandler, NULL));

    if (strlen((char *)wifi_config.sta.password) && FEAT_MODERN_WIFI_ONLY)
    {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "staWifi_init finished. Connecting to network...");

    EventBits_t bits = xEventGroupWaitBits(staWifi_EventGroup,
                                           STA_WIFI_CONNECTED_BIT | STA_WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           (TickType_t)(10000 / portTICK_PERIOD_MS));

    if (bits & STA_WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to network.");
        return true;
    }
    else if (bits & STA_WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect.");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi connection timeout.");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &staWifi_eventHandler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &staWifi_eventHandler));

    vEventGroupDelete(staWifi_EventGroup);

    return false;
}

/***
 * WI-FI FALLBACK MODE
 * */
static void wifiFallback_init(xTimerHandle pxTimer)
{
    static const char *TAG = "wifiFallback_init";

    wifiMode = "Fallback Mode";

    ESP_LOGI(TAG, "Falling back.");

    if (pxTimer != NULL)
    {
        ESP_LOGI(TAG, "Fallback timer expired.");
    }

    // Roll back smart config
    esp_smartconfig_stop();
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 &smartConfig_eventHandler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                 &smartConfig_eventHandler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID,
                                                 &smartConfig_eventHandler));
    ESP_ERROR_CHECK(esp_wifi_disconnect());

    sys_setMacAndHost(WIFI_MODE_AP);

    tcpip_adapter_init();

    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    tcpip_adapter_ip_info_t ip_info;

    IP4_ADDR(&ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    tcpip_adapter_dhcp_status_t status;
    tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_AP, &status);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);

    strcpy(sys_ip, (char *)&ip_info.ip);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiFallback_eventHandler, NULL));

    wifi_config_t wifi_config = {.ap = {.ssid = FALLBACK_SSID,
                                        .ssid_len = strlen(FALLBACK_SSID),
                                        .max_connection = FALLBACK_MAX_CON,
                                        .authmode = WIFI_AUTH_OPEN}};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Fallback mode ready. SSID:%s", FALLBACK_SSID);
}

static void wifiFallback_eventHandler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    static const char *TAG = "wifi_event";

    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event =
            (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event =
            (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
}

/***
 * SMARTCONFIG MODE
 * */
void smartConfig_init(void)
{
    // Don't use ESP_ERROR_CHECK in here. If there's a Wi-Fi connection failure,
    // we should exit this function gracefully instead of aborting.

    static const char *TAG = "smartConfig_wifi";

    wifiMode = "Connected to Wi-Fi via ESP-Touch";

    // Fallback to AP mode if no easy connect activity/success within 2 min.
    if (smartConfig_started == false)
    {
        smartConfig_started = true;
        smartConfig_fail =
            xTimerCreate("Smart Config Timeout Timer", pdMS_TO_TICKS(10000), false,
                         0, &wifiFallback_init);
    }
    else
    {
        ESP_LOGW(TAG, "Fallback timer already exists. Forcing fallback to prevent "
                      "infinite loop.");
        wifiFallback_init(NULL);
    }

    tcpip_adapter_init();
    smartConfig_EventGroup = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &smartConfig_eventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &smartConfig_eventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                               &smartConfig_eventHandler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void smartConfig_taskMgr(void *parm)
{
    static const char *TAG = "smartConfig_taskMgr";

    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SMARTCOFNIG_TYPE));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    while (1)
    {
        uxBits = xEventGroupWaitBits(smartConfig_EventGroup,
                                     SMARTCONFIG_CONNECTED_BIT | SMARTCONFIG_DONE_BIT,
                                     true, false, portMAX_DELAY);

        if (uxBits & SMARTCONFIG_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "Wi-Fi connected to AP.");
        }

        if (uxBits & SMARTCONFIG_DONE_BIT)
        {
            ESP_LOGI(TAG, "Smartconfig done.");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

static void smartConfig_eventHandler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data)
{
    static const char *TAG = "smartConfig_eventHandler";

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        xTaskCreate(smartConfig_taskMgr, "smartConfig_taskMgr", 4096, NULL, 3,
                    NULL);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        ESP_LOGI(TAG, "Connected to WiFi network");
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        xEventGroupClearBits(smartConfig_EventGroup, SMARTCONFIG_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        strcpy(sys_ip, ip4addr_ntoa(&evt->ip_info.ip));

        xEventGroupSetBits(smartConfig_EventGroup, SMARTCONFIG_CONNECTED_BIT);
        xTimerDelete(smartConfig_fail, pdMS_TO_TICKS(5000));
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE)
    {
        ESP_LOGI(TAG, "Scan done");
        xTimerReset(smartConfig_fail, pdMS_TO_TICKS(5000));
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL)
    {
        ESP_LOGI(TAG, "Found channel");
        xTimerReset(smartConfig_fail, pdMS_TO_TICKS(5000));
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD)
    {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt =
            (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password,
               sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;

        if (wifi_config.sta.bssid_set == true)
        {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(smartConfig_ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(smartConfig_password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", smartConfig_ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", smartConfig_password);
        if (SMARTCOFNIG_TYPE == SC_TYPE_ESPTOUCH_V2)
        {
            ESP_ERROR_CHECK(esp_smartconfig_get_rvd_data(
                smartConfig_rcvdData, sizeof(smartConfig_rcvdData)));
            ESP_LOGI(TAG, "OTA URL:%s", smartConfig_rcvdData);
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());

        if (SMARTCOFNIG_TYPE == SC_TYPE_ESPTOUCH_V2)
        {
            // Allow fallback to be called manually.
            if (strcmp((const char *)"fallback",
                       (const char *)smartConfig_rcvdData) == 0)
            {
                ESP_LOGI(TAG, "Manual fallback requested.");
                wifiFallback_init(NULL);
            }
        }
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE)
    {
        xEventGroupSetBits(smartConfig_EventGroup, SMARTCONFIG_DONE_BIT);
    }
}

/**
 * 
 * MDNS FUNCTIONS
 * 
 */

#if CONFIG_IDF_TARGET_ESP8266
static void legacy_event_handler(void *ctx, system_event_t *event)
{
    mdns_handle_system_event(ctx, event);
    // return ESP_OK;
}
#endif

static void init_mdns(void)
{
    char rev[4];
    char cores[4];
    sprintf(rev, "%d", sys_chipInfo.revision);
    sprintf(cores, "%d", sys_chipInfo.cores);

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
        {"chip_rev", rev},
        {"chip_cores", cores},
        {"chip_features", sys_chipInfoFeatures_s}};

    mdns_service_txt_set("_esp2ino", "_tcp", serviceTxtData, 4);
}

/***
 * 
 * LOGGING FUNCTIONS
 * 
 * These functions send messages to esp2ino's web UI.
 * 
 * */

/*
  Sends messages to the frontend using by holding an HTTP response open and returning chunks.
  HTTP request object must be submitted to this task as a parameter.
*/
static esp_err_t webui_msgPub(httpd_req_t *req, char *step, char *state, char *message)
{
    static const char *TAG = "webui_msgPub";
    webMsgQueueSize += 1;

    char *jsonBuffer;

    cJSON *jsonObj = createFlashStatusElement(step, state, message);

    jsonBuffer = cJSON_PrintUnformatted(jsonObj);
    sprintf(jsonBuffer + strlen(jsonBuffer), "\n");

    ESP_LOGD(TAG, "%s", jsonBuffer);

    httpd_resp_send_chunk(req, jsonBuffer, -1);

    webMsgQueueSize -= 1;

    return ESP_OK;
}

/*
  When activated, this redirects the ESP_LOGX logging functions to the web UI. Characters are sent
  here one at a time and are dumped to output on newline. This function then forwards
  chars to RTOS' built-in UART logger.
  https://github.com/mriksman/esp88266-sse-ota/blob/master/main/main.c

  This is disabled because it creates a race condition that results in a system crash and bad UX.
  old_logger seems to run asynchronously in a way that fails to hold the handleFlash connection
  open. After a successful flash, the system will attempt to restart before all webui_debugPub and
  webui_msgPub messages have been sent. This triggers FreeRTOS' watchdog and, therefore, a system
  crash. The user will experience the webui's flash status tracker get stuck.
*/
#if FEAT_DEBUG_LOG_TO_WEBUI
int webui_debugPub(int chr)
{
    char *jsonBuffer;
    // static const char *TAG = "webui_debugPub";
    size_t len = strlen(debug_logBuffer);
    if (chr == '\n')
    {
        webMsgQueueSize += 1;

        if (len < LOG_BUF_MAX_LINE_SIZE - 1)
        {
            debug_logBuffer[len] = chr;
            debug_logBuffer[len + 1] = '\0';
        }
        // send without the '\n'

        cJSON *jsonObj = createDebugStatusElement(debug_logBuffer);

        jsonBuffer = cJSON_PrintUnformatted(jsonObj);
        sprintf(jsonBuffer + strlen(jsonBuffer), "\n");

        // ESP_LOGI(TAG, "%s", jsonBuffer);

        httpd_resp_send_chunk(flash_httpdReq, jsonBuffer, strlen(jsonBuffer));

        // httpd_resp_send_chunk(flash_httpdReq, debug_logBuffer, len);
        // 'clear' string
        debug_logBuffer[0] = '\0';
        webMsgQueueSize -= 1;
    }
    else
    {
        if (len < LOG_BUF_MAX_LINE_SIZE - 1)
        {
            debug_logBuffer[len] = chr;
            debug_logBuffer[len + 1] = '\0';
        }
    }

    // Still send to console
    return old_logger(chr);
}
#endif /* FEAT_DEBUG_LOG_TO_WEBUI */

/***
 * 
 * HTTP SERVER
 * 
 * */

/**
 * URI Handlers
 */
httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handleRoot};
httpd_uri_t flash = {.uri = "/flash", .method = HTTP_GET, .handler = handleFlash};
httpd_uri_t status = {.uri = "/info", .method = HTTP_GET, .handler = handleInfo};
httpd_uri_t backup = {.uri = "/backup", .method = HTTP_GET, .handler = handleBackup};
httpd_uri_t undo = {.uri = "/undo", .method = HTTP_GET, .handler = handleUndo};
httpd_uri_t upload = {.uri = "/upload", .method = HTTP_POST, .handler = handleUpload};

/**
 * Event handler for HTTP CLIENT. Used by OTA handler when fetching firmware. 
 * ENHANCEMENT: Use this to show download progress for firmware.
 *              Also to confirm that image will fit on partition.
 */
esp_err_t _ota_http_event_handler(esp_http_client_event_t *evt)
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

esp_err_t handleRoot(httpd_req_t *req)
{
    // Reloading the entire page. sysInfo needs to re-initialize everything.
    sysInfoTransmitted = false;

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, index_html_gz, index_html_gz_len);

    return ESP_OK;
}

esp_err_t handleUpload(httpd_req_t *req)
{
    static const char *TAG = "handleUpload";
    char buf[100];
    int ret, remaining = req->content_len;

    while (remaining > 0)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf,
                                  MIN(remaining, sizeof(buf)))) <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        // httpd_resp_send_chunk(req, buf, ret);

        remaining -= ret;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA (%i Remains) ==========", remaining);
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/**
 * This function retrieves flash size from image headers, not from SPI chip
 * itself.
 * */
esp_err_t handleBackup(httpd_req_t *req)
{
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

esp_err_t handleFlash(httpd_req_t *req)
{
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

    if (doFlash(req) == ESP_OK)
    {
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

        while (webMsgQueueSize > 0)
        {
            /* Wait for all webui messages to be published. */
            ESP_LOGI(TAG, "Messages in Web UI Queue: %d", webMsgQueueSize);
        }

        ESP_LOGI(TAG, "Restarting....");

#if FEAT_DEBUG_LOG_TO_WEBUI
        /** Disable Log to Web **/
        esp_log_set_putchar(old_logger);
#endif

        /** Close HTTP Response **/
        httpd_resp_send_chunk(req, NULL, 0);

        esp_restart();
    }
    else
    {
        sprintf(httpRespBuffer, "Flashing %s failed.", user_url_buf);
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

esp_err_t handleUndo(httpd_req_t *req)
{
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
            sprintf(httpRespBuffer, "Undone successfully. Rebooting...");
            ESP_LOGI(TAG, "%s", httpRespBuffer);
            for (int i = 4; i >= 0; i--)
            {
                ESP_LOGI(TAG, "Restarting in %d seconds...", i);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        }
    }

    httpd_resp_send(req, httpRespBuffer, strlen(httpRespBuffer));

    esp_restart();

    return ESP_OK;
}

esp_err_t handleInfo(httpd_req_t *req)
{
    memset(httpRespBuffer, 0, sizeof httpRespBuffer);

    char client_ipstr[INET6_ADDRSTRLEN];
    req_getClientIp(req, client_ipstr);

    const char *FlashSize = "";
    switch (flash_dataAddr[3] & 0xF0)
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
    switch (flash_dataAddr[2] & 0xF)
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
    switch (flash_dataAddr[3] & 0xF)
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

    cJSON *elements = NULL;
    cJSON *element = NULL;
    elements = cJSON_CreateArray();
    char *jsonBuffer;

    sprintf(httpRespBuffer, "%s", (char *)sys_mac_str);
    element = createGenStatusElement("mac", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s", wifiMode);
    element = createGenStatusElement("wifi_mode", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s", client_ipstr);
    element = createGenStatusElement("client_ip", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s %s @ %sMHz", FlashSize, FlashMode, FlashSpeed);
    element = createGenStatusElement("flash_mode", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s @ 0x%06x (%u bytes)", sys_partConfigured->label,
            sys_partConfigured->address, sys_partConfigured->size);
    element = createGenStatusElement("part_boot_conf", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s @ 0x%06x (%u bytes)", sys_partRunning->label, sys_partRunning->address, sys_partRunning->size);
    element = createGenStatusElement("part_boot_act", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s @ 0x%06x (%u bytes)", sys_partIdle->label, sys_partIdle->address, sys_partIdle->size);
    element = createGenStatusElement("part_idle", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s",
            (flash_erasedFactoryApp ? "Third Party" : "Factory"));
    element = createGenStatusElement("part_idle_content", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s",
            (flash_erasedBootloader ? "Arduino eboot (Third Party)"
                                    : "Espressif (Factory)"));
    element = createGenStatusElement("bootloader", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s Core %s v%s (%s)", sys_chipInfo.cores == 1 ? "Single" : sys_chipInfoCores_s, sys_chipInfoModel_s, sys_chipInfoRev_s, sys_chipInfoFeatures_s);
    element = createGenStatusElement("soc", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(httpRespBuffer, "%s", sys_chipInfoModel_s);
    element = createGenStatusElement("chip", httpRespBuffer);
    cJSON_AddItemToArray(elements, element);

    /** 
     * This URI is called on the first load of the index page, then again
     * if a flashing step fails. We only want to initialize the debug log
     * on the index page's initial load.
     * */
    if (!sysInfoTransmitted)
    {
        element = createGenStatusElement("debug-log", "<pre>Ready to flash...</pre>");
        cJSON_AddItemToArray(elements, element);
        sysInfoTransmitted = true;
    }

    jsonBuffer = cJSON_Print(elements);

    httpd_resp_send(req, jsonBuffer, strlen(jsonBuffer));

    free(jsonBuffer);
    free(elements);

    return ESP_OK;
}

void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;

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
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

/***
 * 
 * 
 * CORE FUNCTIONS
 * 
 * 
 * */

/***
 * 
 * FLASHER
 * 
 * This function performs the core write/erase functions
 * related to replacing the device's firmware and bootloader.
 * 
 * This task is started by the http server's handleFlash (/flash) handler.
 *
 * */

esp_err_t flash_viaUpload(httpd_req_t *req)
{
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;
    ESP_LOGI(TAG, "Starting OTA...");
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
    while (1)
    {
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, upgrade_data_buf,
                                  MIN(remaining, sizeof(upgrade_data_buf)))) <= 0)
        {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
            {
                /* Retry receiving if timeout occurred */
                continue;
            }
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
                break;
            }
            remaining -= ret;
            ESP_LOGD(TAG, "Remaining %d", remaining);
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

esp_err_t flash_viaDownload(httpd_req_t *req)
{
    static const char *TAG = "flashViaDownload";
    esp_err_t ret;

    ESP_LOGI(TAG, "BEGIN: Downloading and flashing third party bin.");

    /** Report to Web UI [Task 1 of 5] **/
    webui_msgPub(req, "1", FLASH_STATE_RUNNING, "");

    esp_http_client_config_t config = {.url = user_url_buf,
                                       .event_handler = _ota_http_event_handler};

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
            sprintf(flashTsk_logBuffer, "Failed to download firmware. Trying again. Attempt %d of 5.", attempt + 1);
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
            flash_erasedFactoryApp = true;

            /** Report to Web UI [Task 1 of 5] **/
            webui_msgPub(req, "1", FLASH_STATE_SUCCESS, "");

            break;
        }
        else
        {
            return ESP_FAIL;
        }

        ++attempt;
    }

    ESP_LOGI(TAG, "DONE: Downloading and flashing third party bin.");

    return ESP_OK;
}

static esp_err_t doFlash(httpd_req_t *req)
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

    ret = flash_viaDownload(req);

    if (ret != ESP_OK)
    {
        /** Report to Web UI [Task 1 of 5] **/
        webui_msgPub(req, "1", FLASH_STATE_FAIL, "Failed while installing firmware. This should be recoverable. Try again after verifying that your "
                                                 "internet connection is working and that your firmware URL is correct. "
                                                 "<br><br>⚠️ DO NOT REVERT TO FACTORY FIRMWARE! ⚠️<br><br>"
                                                 "Factory firmware may have been overwritten during this flashing attempt. "
                                                 "Reverting to this overwritten firmware will brick your device. You can reboot / power cycle your device if needed to return to esp2ino.");

        flash_erasedFactoryApp = true;
        return ret;
    }

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
    eboot_bin_ptr[2] = flash_dataAddr[2];
    eboot_bin_ptr[3] = flash_dataAddr[3];

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
void sys_setMacAndHost(int wifiMode)
{
    esp_mac_type_t macType = (wifiMode == WIFI_MODE_STA) ? ESP_MAC_WIFI_STA : ESP_MAC_WIFI_SOFTAP;

    esp_read_mac(sys_mac, macType);

    snprintf(sys_mac_str, sizeof(sys_mac_str), MACSTR, sys_mac[0],
             sys_mac[1], sys_mac[2], sys_mac[3], sys_mac[4], sys_mac[5]);

    ESP_LOGI(TAG, "MAC Address: %s", sys_mac_str);

#if FEAT_MAC_IN_MDNS_HOSTNAME
    // Generate sys_hostname
    sprintf(sys_hostname, "esp2ino-%02X%02X%02X", sys_mac[3], sys_mac[4], sys_mac[5]);
    ESP_LOGI(TAG, "mdns sys_hostname: %s", sys_hostname);
#else
    sprintf(sys_hostname, "esp2ino");
    ESP_LOGI(TAG, "mdns sys_hostname: %s", sys_hostname);
#endif
}

int req_getClientIp(httpd_req_t *req, char ipstr[40])
{
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

cJSON *createGenStatusElement(char *id, char *innerHTML)
{
    cJSON *element;
    element = cJSON_CreateObject();
    cJSON *jsonId;
    cJSON *jsonHTML;

    jsonId = cJSON_CreateString(id);
    jsonHTML = cJSON_CreateString(innerHTML);

    cJSON_AddItemToObject(element, "id", jsonId);
    cJSON_AddItemToObject(element, "innerHTML", jsonHTML);
    return element;
}

cJSON *createFlashStatusElement(char *stepNum, char *state, char *msg)
{
    cJSON *element;
    element = cJSON_CreateObject();

    cJSON *jsonMsg;
    cJSON *jsonStepNum;
    cJSON *jsonState;
    cJSON *jsonType;

    jsonMsg = cJSON_CreateString(msg);
    jsonStepNum = cJSON_CreateString(stepNum);
    jsonState = cJSON_CreateString(state);
    jsonType = cJSON_CreateString("flashStatus");

    cJSON_AddItemToObject(element, "step", jsonStepNum);
    cJSON_AddItemToObject(element, "state", jsonState);
    cJSON_AddItemToObject(element, "message", jsonMsg);
    cJSON_AddItemToObject(element, "type", jsonType);

    return element;
}

cJSON *createDebugStatusElement(char *msg)
{
    cJSON *element;
    element = cJSON_CreateObject();

    cJSON *jsonMsg;
    cJSON *jsonType;

    jsonMsg = cJSON_CreateString(msg);
    jsonType = cJSON_CreateString("debug");

    cJSON_AddItemToObject(element, "message", jsonMsg);
    cJSON_AddItemToObject(element, "type", jsonType);

    return element;
}

void app_main()
{
    static const char *TAG = "main";

    // Use 115200 for compatibility with gdb.
    uart_set_baudrate(0, 115200);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(nvs_flash_init());

    // Start ESP-IDF-style event loop.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_IDF_TARGET_ESP8266
    // Register mDNS with the legacy event loop as per https://github.com/espressif/ESP8266_RTOS_SDK/issues/870.
    esp_event_handler_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID, &legacy_event_handler, NULL);
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

    spi_flash_read(0x0, (uint32_t *)flash_dataAddr, 4);
    ESP_LOGI(TAG,
             "Magic: %02x, Segments: %02x, Flash Mode: %02x, Size/Freq: %02x",
             flash_dataAddr[0], flash_dataAddr[1], flash_dataAddr[2],
             flash_dataAddr[3]);

    // ENHANCEMENT: Use esp_partition_find to confirm assumptions about flash layout and to get actual chip size.
    sys_realFlashSize = spi_flash_get_chip_size();

    esp_chip_info(&sys_chipInfo);

    sprintf(sys_chipInfoCores_s, "%d", sys_chipInfo.cores);
    sprintf(sys_chipInfoRev_s, "%d", sys_chipInfo.revision);

    sys_chipInfoModel_s = "ESP8266";

    ESP_LOGI(TAG, "%s", sys_chipInfoModel_s);

    sprintf(sys_chipInfoFeatures_s, "%s%s%s%s",
            sys_chipInfo.features & CHIP_FEATURE_WIFI_BGN ? "802.11bgn " : "",
            sys_chipInfo.features & CHIP_FEATURE_BLE ? "BLE " : "",
            sys_chipInfo.features & CHIP_FEATURE_BT ? "BT " : "",
            sys_chipInfo.features & CHIP_FEATURE_EMB_FLASH ? "Embedded-Flash" : "External-Flash");

    // This gives us a MAC address and hostname to use immediately.
    // Will be called again if we enter fallback mode. (AP/STA modes have different MACs).
    sys_setMacAndHost(WIFI_MODE_STA);

    init_mdns();

    ESP_LOGI(TAG, "DONE: Retrieving system data.");

    ESP_LOGI(TAG, "BEGIN: Start AP and web server.");

    ESP_LOGI(TAG, "Starting Wi-Fi.");

    /* Try to connect using stored credentials. */
    if (staWifi_init() == false)
    {
        ESP_LOGI(TAG, "Couldn't connect using stored credentials. Starting smartconfig.");
        smartConfig_init();
    }

    if (server == NULL)
    {
        ESP_LOGI(TAG, "Starting webserver");
        server = start_webserver();
    }

    ESP_LOGI(TAG, "DONE: Start AP and web server.");

    ESP_LOGI(TAG, "Initialization complete. Waiting for user command.");

    strcpy(sys_statusMsg, "Ready to flash.");
}