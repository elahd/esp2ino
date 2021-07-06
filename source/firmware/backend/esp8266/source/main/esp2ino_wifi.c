#include "esp2ino_common.h"

char *wifi__mode = "Unknown";
bool wifi__isConnected = false;

static int connectRetryNum;
static const wifi_config_t emptyStruct;

static EventGroupHandle_t staWifi_EventGroup;
static EventGroupHandle_t wifiScan_EventGroup;

static bool staWifiCreds_getWyze(wifi_config_t *wifi_config);
static bool staWifiCreds_getEsp(wifi_config_t *wifi_config);
static void wifi_eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

/**
 * Loads Wi-Fi credentials stored by Wyze firmware.
 * wifi_config must be 0 initialized.
 */
static bool staWifiCreds_getWyze(wifi_config_t *wifi_config)
{
    static const char *TAG = "wifi__staWifiCreds_getWyze";

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
static bool staWifiCreds_getEsp(wifi_config_t *wifi_config)
{
    static const char *TAG = "wifi__staWifiCreds_getEsp";

    ESP_LOGI(TAG, "Searching for Wi-Fi credendials saved by Espressif's Wi-Fi libraries.");

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

    return ret;
}

bool wifi__apSta_init(void)
{
    // Don't use ESP_ERROR_CHECK in here. If there's a Wi-Fi connection failure,
    // we should exit this function gracefully instead of aborting.

    static const char *TAG = "wifi__apSta_init";

    staWifi_EventGroup = xEventGroupCreate();

    tcpip_adapter_init();

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_init(&cfg));
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Load Stored WiFi Config */
    wifi_config_t wifi_config_sta;
    if (staWifiCreds_getEsp(&wifi_config_sta))
    {
        wifi__mode = "Retrieved Wi-Fi Credentials via Espressif";
    }
    else if (staWifiCreds_getWyze(&wifi_config_sta))
    {
        wifi__mode = "Retrieved Wi-Fi Credentials via Wyze";
    }
    else
    {
        wifi__mode = "Not connected to Wi-Fi network.";
    }

    ESP2INO_ERROR_RETURN_FALSE(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_eventHandler, NULL));
    ESP2INO_ERROR_RETURN_FALSE(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_eventHandler, NULL));

    if (strlen((char *)wifi_config_sta.sta.password) && FEAT_MODERN_WIFI_ONLY)
    {
        wifi_config_sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    /** Configure AP **/
    tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
    tcpip_adapter_ip_info_t ap_ip_info;

    IP4_ADDR(&ap_ip_info.ip, 10, 0, 0, 1);
    IP4_ADDR(&ap_ip_info.gw, 10, 0, 0, 1);
    IP4_ADDR(&ap_ip_info.netmask, 255, 255, 255, 0);

    tcpip_adapter_dhcp_status_t status;
    tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_AP, &status);
    tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ap_ip_info);
    tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);

    // strcpy(wifi__ip_ap, (char *)&ap_ip_info.ip);
    strlcpy(wifi__ip_ap, ip4addr_ntoa(&ap_ip_info.ip), sizeof(wifi__ip_ap));

    wifi_config_t wifi_config_ap = {.ap = {.ssid = FALLBACK_SSID,
                                           .ssid_len = strlen(FALLBACK_SSID),
                                           .max_connection = FALLBACK_MAX_CON,
                                           .authmode = WIFI_AUTH_OPEN}};

    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta));
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_start());

    ESP_LOGI(TAG, "wifi__apSta_init finished. Connecting to network...");

    EventBits_t bits = xEventGroupWaitBits(staWifi_EventGroup,
                                           STA_WIFI_CONNECTED_BIT | STA_WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           (TickType_t)(60000 / portTICK_PERIOD_MS));

    if (bits && STA_WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "Connected to network.");
        vEventGroupDelete(staWifi_EventGroup);
        return true;
    }
    else if (bits && STA_WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect.");
    }
    else
    {
        ESP_LOGW(TAG, "Wi-Fi connection timeout.");
    }

    vEventGroupDelete(staWifi_EventGroup);

    return false;
}

static void wifi_eventHandler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    static const char *TAG = "wifi__eventHandler";

    if (!connectRetryNum)
        connectRetryNum = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event =
            (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event =
            (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
                 event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
        memcpy(wifi__staSsid, event->ssid, sizeof(wifi__staSsid));
        memcpy(wifi__staBssid, event->bssid, sizeof(wifi__staBssid));
        snprintf(wifi__channel, sizeof(wifi__channel), "%u", event->channel);
        wifi__isConnected = true;
        if (wifi__connectSemaphore != NULL)
        {
            xSemaphoreGive(wifi__connectSemaphore);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        memset(wifi__staSsid, 0, sizeof wifi__staSsid);
        memset(wifi__staBssid, 0, sizeof wifi__staBssid);
        memset(wifi__channel, 0, sizeof wifi__channel);
        wifi__isConnected = false;

        if (connectRetryNum < STA_WIFI_NUM_ATTEMPTS)
        {
            esp_wifi_connect();
            connectRetryNum++;
            ESP_LOGI(TAG, "Not connected to AP. Retrying (%d of %d)...", connectRetryNum, STA_WIFI_NUM_ATTEMPTS);
        }
        else
        {
            ESP_LOGI(TAG, "Not connected to AP. Giving up.");
            connectRetryNum = 0;
            if (wifi__connectSemaphore != NULL)
            {
                xSemaphoreGive(wifi__connectSemaphore);
            }
            xEventGroupSetBits(staWifi_EventGroup, STA_WIFI_FAIL_BIT);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        strlcpy(wifi__ip_sta, ip4addr_ntoa(&event->ip_info.ip), sizeof(wifi__ip_sta));
        ESP_LOGI(TAG, "Got IP:%s", wifi__ip_sta);
        connectRetryNum = 0;
        xEventGroupSetBits(staWifi_EventGroup, STA_WIFI_CONNECTED_BIT);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE)
    {
        xEventGroupSetBits(wifiScan_EventGroup, STA_WIFI_SCAN_RESULTS_BIT);

        ESP_LOGI(TAG, "sta scan done");
    }
}

esp_err_t wifi__apStaScanHelper(httpd_req_t *req, char *respBuffer, size_t respBufferSize)
{
    static const char *TAG = "wifi___apStaScanHelper";

    uint16_t sta_number = 0;
    wifi_scan_config_t scan_config = {0};
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Content-Type", "application/json");

    wifiScan_EventGroup = xEventGroupCreate();

    if (xEventGroupGetBits(wifiScan_EventGroup) == STA_WIFI_SCAN_RESULTS_BIT)
    {
        ESP_LOGI(TAG, "Wi-Fi scan already in progress.");
        return ESP_FAIL;
    }
    else if (esp_wifi_scan_start(&scan_config, true) != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to start Wi-Fi scan.");
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(wifiScan_EventGroup,
                                           STA_WIFI_SCAN_RESULTS_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           (TickType_t)(10000 / portTICK_PERIOD_MS));

    if (bits & STA_WIFI_SCAN_RESULTS_BIT)
    {
        // Replaced by pdTRUE for xClearOnExit above.
        // xEventGroupClearBits(wifiScan_EventGroup, STA_WIFI_SCAN_RESULTS_BIT);

        if (esp_wifi_scan_get_ap_num(&sta_number) != ESP_OK)
        {
            return ESP_FAIL;
        }

        wifi_ap_record_t *ap_list_buffer = malloc(sta_number * sizeof(wifi_ap_record_t));
        if (ap_list_buffer == NULL)
        {
            ESP_LOGE(TAG, "Failed to malloc buffer to print scan results");
            return ESP_FAIL;
        }

        if (esp_wifi_scan_get_ap_records(&sta_number, (wifi_ap_record_t *)ap_list_buffer) == ESP_OK)
        {
            httpd_resp_send_chunk(req, "{\"type\":\"wifi_results\", \"wifi_results\":", -1);
            httpd_resp_send_chunk(req, "[", -1);
            uint8_t i;
            for (i = 0; i < sta_number; i++)
            {
                int wifi_signal = MIN(MAX(2 * (ap_list_buffer[i].rssi + 100), 0), 100);

                char ssid_str[32];
                memcpy(ssid_str, ap_list_buffer[i].ssid, 32);

                char signal_str[16];
                sprintf(signal_str, "%d", wifi_signal);

                char channel_str[16];
                sprintf(channel_str, "%d", ap_list_buffer[i].primary);

                char bssid_str[18];
                sprintf(bssid_str, MACSTR, MAC2STR(ap_list_buffer[i].bssid));

                snprintf(respBuffer, respBufferSize,
                         "{\"ssid\": \"%s\","
                         "\"signal\": %s,"
                         "\"channel\": %s,"
                         "\"bssid\": \"%s\"}"
                         "%s",
                         ssid_str, signal_str, channel_str, bssid_str,
                         ((i == sta_number - 1) ? "" : ","));

                httpd_resp_send_chunk(req, respBuffer, -1);
            }
            httpd_resp_send_chunk(req, "]}", -1);
            free(ap_list_buffer);

            return ESP_OK;
        }
        free(ap_list_buffer);
    }

    return ESP_FAIL;
}

esp_err_t wifi__apStaSetHelper(httpd_req_t *req, char param_ssid[32], char param_pswd[64])
{
    static const char *TAG = "wifi___apStaSetHelper";

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send_chunk(req, NULL, 0);

    staWifi_EventGroup = xEventGroupCreate();

    wifi_config_t wifi_config_sta;

    bzero(&wifi_config_sta, sizeof(wifi_config_t));

    memcpy(wifi_config_sta.sta.ssid, param_ssid, sizeof(wifi_config_sta.sta.ssid));
    memcpy(wifi_config_sta.sta.password, param_pswd, sizeof(wifi_config_sta.sta.password));
    wifi_config_sta.sta.bssid_set = false;

    ESP_LOGW(TAG, "SSID: %s, PASS: %s", wifi_config_sta.sta.ssid, wifi_config_sta.sta.password);

    // snprintf(httpRespBuffer, sizeof(httpRespBuffer),
    //          "{\"type\":\"wifi_set_success\","
    //          "\"ip\":\"%s\"}",
    //          wifi__ip_sta);
    // httpd_resp_send_chunk(req, httpRespBuffer, -1);

    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_disconnect());
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config_sta));
    ESP2INO_ERROR_RETURN_FALSE(esp_wifi_connect());

    return ESP_OK;
}