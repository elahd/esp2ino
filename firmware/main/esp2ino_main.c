#include "esp2ino_main.h"
#include "eboot_command.h"

#include "../static/eboot_bin.h"
#include "../static/index.html.gz.h"

/** Pre-Processor Directives & Variables  **/
#define DEVMODE true

#define LOG_NORM 0x0
#define LOG_WARN 0x1
#define LOG_FAIL 0x2
#define LOG_SUCCESS 0x3
#define LOG_BYE 0x4

#define FAIL_INIT 0x10
#define FAIL_APP_OTA 0x20
#define FAIL_BOOT_ERASE 0x31
#define FAIL_BOOT_WRITE 0x32
#define FAIL_EBOOT 0x40
#define FAIL 0x99
#define SUCCESS 0x00

#define SPI_FLASH_SEC_SIZE 4096

#define HTTP_RETRIES 10
#define HTTP_RETRY_WAIT 10

#define OTA_0_ADDR 0x010000
#define OTA_1_ADDR 0x110000
#define OTA_BUF_SIZE 250
#define OTA_MAX_SIZE 0x40000

const esp_partition_t *part_running;
const esp_partition_t *part_configured;
const esp_partition_t *part_idle;

uint32_t flash_data;
uint8_t *flash_data_addr = (uint8_t *)&flash_data;
static unsigned int real_flash_size;

static const uint32_t s_log_color[5] = {
    37, //  LOG_NORM White
    93, //  LOG_WARN Bright Yellow
    91, //  LOG_FAIL Bright Red
    92, //  LOG_SUCCESS Bright Green
    93, //  LOG_BYE Bright Yellow
};

typedef struct flash_conf
{
    char *url;
    httpd_req_t *req;
} flash_conf;
flash_conf fc;

QueueHandle_t taskMessages;
char buffer[SPI_FLASH_SEC_SIZE * 4] __attribute__((aligned(4))) = {0};
char df_log_buffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4))) = {0};
char fh_log_buffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4))) = {0};

char http_last_modified[50];
static httpd_handle_t server = NULL;
uint8_t mac[6];
char macStr[18];
char bssidMacStr[18];
char status_txt[100];

static EventGroupHandle_t flash_event_group;
static const int FLASH_SUCCESS_BIT = BIT2;
static const int FLASH_FAIL_BIT = BIT3;

bool erased_bootloader = false;
bool erased_factory_app = false;

static putchar_like_t old_logger = NULL;
bool output_esplog_to_web = false;
#define LOG_BUF_MAX_LINE_SIZE 250
char log_httpd_buf[LOG_BUF_MAX_LINE_SIZE];
httpd_req_t *log_httpd_req;

/***
 * ESP-TOUCH
 * */
char ip[32];
bool smartconfig_started = false;
static EventGroupHandle_t s_wifi_event_group;
TimerHandle_t smartconfig_fail;
uint8_t smartconfig_ssid[250] = {0};
uint8_t smartconfig_password[250] = {0};
uint8_t smartconfig_rvd_data[250] = {0};

#define ESP_SMARTCOFNIG_TYPE SC_TYPE_ESPTOUCH

static const int ESPTOUCH_CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

#define WIFI_SSID "esp2ino"
#define WIFI_MAX_CON 10

// TO-DO:
// Handle root last request time.
// Try to connect to stored wifi.
// test with something other than tasmota
// Test with esp-touch

static const char *TAG = "global";

/***
 * 
 * FUNCTIONS
 * 
 * */

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
static esp_err_t webui_msgPub(httpd_req_t *req, char *intro, char *message, int msgType)
{
    static const char *TAG = "webui_msgPub";

    int length = 0;

    char *msgSpace_txt = "";
    msgSpace_txt = (strcmp(intro, "") != 0) ? " " : "";

    length = sprintf(log_httpd_buf,
                     "\033[1;%dm%s\033[0m%s\033[0;%dm%s\033[0m\n",
                     s_log_color[msgType], intro, msgSpace_txt, s_log_color[msgType], message);

    ESP_LOGI(TAG, log_httpd_buf);

    ESP_ERROR_CHECK(httpd_resp_send_chunk(req, log_httpd_buf, length));

    if (msgType == LOG_BYE)
    {
        // Triggers confetti effect on frontend.
        ESP_ERROR_CHECK(httpd_resp_send_chunk(req, "🎉", strlen("🎉")));
    }

    return ESP_OK;
}

/*
  When activated, this redirects the ESP_LOGX logging functions. Characters are sent
  here one at a time and are dumped to output on newline. This function then forwards
  chars to RTOS' built-in UART logger.
  https://github.com/mriksman/esp88266-sse-ota/blob/master/main/main.c
*/
int webui_debugPub(int chr)
{
    // static const char *TAG = "webui_debugPub";
    size_t len = strlen(log_httpd_buf);
    if (chr == '\n')
    {
        if (len < LOG_BUF_MAX_LINE_SIZE - 1)
        {
            log_httpd_buf[len] = chr;
            log_httpd_buf[len + 1] = '\0';
        }
        // send without the '\n'
        httpd_resp_send_chunk(log_httpd_req, log_httpd_buf, len);
        // 'clear' string
        log_httpd_buf[0] = '\0';
    }
    else
    {
        if (len < LOG_BUF_MAX_LINE_SIZE - 1)
        {
            log_httpd_buf[len] = chr;
            log_httpd_buf[len + 1] = '\0';
        }
    }

    // Still send to console
    return old_logger(chr);
}

/***
 * WIFI WORKFLOW
 * 
 * 1) [SMARTCONFIG] esp2ino first tries to establish a connection using espressif esptouch / smartconfig.
 *    If a connection can't be made within 2 minutes, the firmware will enter "fallback mode."
 * 
 * 2) [FALLBACK MODE] The firmware will create a WiFi network named esp2ino. Clients can connect to this
 *    network to install firmware from a server on their local computer.
 * 
 * 3) [FALLBACK TIMER] The smartconfig event handler resets the 2 minute fallback timer several times
 *    while trying to connect. This prevents us from falling back when Wi-Fi is available, but slow to
 *    connect.
 * */

/***
 * WI-FI FALLBACK
 * */
static void wifiFallback_init(xTimerHandle pxTimer)
{
    static const char *TAG = "wifiFallback_init";

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

    strcpy(ip, (char *)&ip_info.ip);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiFallback_eventHandler, NULL));

    wifi_config_t wifi_config = {.ap = {.ssid = WIFI_SSID,
                                        .ssid_len = strlen(WIFI_SSID),
                                        .max_connection = WIFI_MAX_CON,
                                        .authmode = WIFI_AUTH_OPEN}};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Fallback mode ready. SSID:%s", WIFI_SSID);
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
 * ESP-TOUCH SMARTCONFIG
 * */
static void smartConfig_init(void)
{
    static const char *TAG = "smartconfig_wifi";

    // Fallback to AP mode if no easy connect activity/success within 2 min.
    if (smartconfig_started == false)
    {
        smartconfig_started = true;
        smartconfig_fail =
            xTimerCreate("Smart Config Timeout Timer", pdMS_TO_TICKS(120000), false,
                         0, &wifiFallback_init);
    }
    else
    {
        ESP_LOGW(TAG, "Fallback timer already exists. Forcing fallback to prevent "
                      "infinite loop.");
        wifiFallback_init(NULL);
    }

    tcpip_adapter_init();
    s_wifi_event_group = xEventGroupCreate();

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
    static const char *TAG = "smartconfig";

    EventBits_t uxBits;
    ESP_ERROR_CHECK(esp_smartconfig_set_type(ESP_SMARTCOFNIG_TYPE));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    while (1)
    {
        uxBits = xEventGroupWaitBits(s_wifi_event_group,
                                     ESPTOUCH_CONNECTED_BIT | ESPTOUCH_DONE_BIT,
                                     true, false, portMAX_DELAY);

        if (uxBits & ESPTOUCH_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "Wi-Fi connected to AP.");
        }

        if (uxBits & ESPTOUCH_DONE_BIT)
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
    static const char *TAG = "smartconfig_event_handle";

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        xTaskCreate(smartConfig_taskMgr, "smartConfig_taskMgr", 4096, NULL, 3,
                    NULL);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        if (strcmp((const char *)smartconfig_rvd_data, "") != 0)
        {
            // TO-DO: When connected, look for fallback demand, otherwise do nothing.
            // Should we remove this feature altogether and stay with ESP-TOUCH v1?
            // init_smartconfig_flash((char*)smartconfig_rvd_data);
        }
        else
        {
            ESP_LOGI(TAG, "No firmware URL provided. Connecting to Wi-Fi and waiting "
                          "for user input.");
        }
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, ESPTOUCH_CONNECTED_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        strcpy(ip, ip4addr_ntoa(&evt->ip_info.ip));

        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_CONNECTED_BIT);
        xTimerDelete(smartconfig_fail, pdMS_TO_TICKS(5000));
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE)
    {
        ESP_LOGI(TAG, "Scan done");
        xTimerReset(smartconfig_fail, pdMS_TO_TICKS(5000));
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL)
    {
        ESP_LOGI(TAG, "Found channel");
        xTimerReset(smartconfig_fail, pdMS_TO_TICKS(5000));
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

        memcpy(smartconfig_ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(smartconfig_password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", smartconfig_ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", smartconfig_password);
        if (ESP_SMARTCOFNIG_TYPE == SC_TYPE_ESPTOUCH_V2)
        {
            ESP_ERROR_CHECK(esp_smartconfig_get_rvd_data(
                smartconfig_rvd_data, sizeof(smartconfig_rvd_data)));
            ESP_LOGI(TAG, "OTA URL:%s", smartconfig_rvd_data);
        }

        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());

        if (ESP_SMARTCOFNIG_TYPE == SC_TYPE_ESPTOUCH_V2)
        {
            // Allow fallback to be called manually.
            if (strcmp((const char *)"fallback",
                       (const char *)smartconfig_rvd_data) == 0)
            {
                ESP_LOGI(TAG, "Manual fallback requested.");
                wifiFallback_init(NULL);
            }
        }
    }
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE)
    {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

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

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
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

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Last-Modified", http_last_modified);
    httpd_resp_send(req, index_html_gz, index_html_gz_len);

    return ESP_OK;
}

/**
 * This function retrieves flash size from image headers, not from SPI chip
 * itself.
 * */
esp_err_t handleBackup(httpd_req_t *req)
{
    ESP_LOGI(TAG, "BEGIN: Sending backup.");

    char buf[OTA_BUF_SIZE];
    char hdr_disp_buf[150];
    char hdr_len_buf[150];

    sprintf(hdr_disp_buf, "attachment; filename=\"firmware-%s.bin\"", macStr);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr_disp_buf);

    sprintf(hdr_len_buf, "%d", real_flash_size);
    httpd_resp_set_hdr(req, "Content-Length", hdr_len_buf);

    httpd_resp_set_type(req, HTTPD_TYPE_OCTET);

    int read = 0;
    while (read < real_flash_size)
    {
        spi_flash_read(read, (char *)buf, OTA_BUF_SIZE);
        httpd_resp_send_chunk(req, buf, OTA_BUF_SIZE);
        read += OTA_BUF_SIZE;
    }

    httpd_resp_send_chunk(req, NULL, 0);

    ESP_LOGI(TAG, "DONE: Sending backup.");

    return ESP_OK;
}

esp_err_t handleFlash(httpd_req_t *req)
{
    memset(buffer, 0, sizeof buffer);
    char *buf;
    size_t buf_len;
    EventBits_t uxBits;
    esp_err_t tmRet;
    flash_event_group = xEventGroupCreate();

    char user_url_buf[200];
    char p_output_debug[4];

    char client_ipstr[40];
    req_get_client_ip(req, client_ipstr);

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
            if (httpd_query_key_value(buf, "outputDebug", p_output_debug,
                                      sizeof(p_output_debug)) == ESP_OK)
            {
                ESP_LOGI(TAG, "Found debug output parameter => outputDebug=%s",
                         p_output_debug);
                if (strcmp(p_output_debug, "1"))
                {
                    output_esplog_to_web = true;
                }
            }
        }
        free(buf);
    }

    /** Enable Log to Web **/
    if (output_esplog_to_web)
    {
        old_logger = esp_log_set_putchar(&webui_debugPub);
    }

    if (user_url_buf == 0 || strcmp(user_url_buf, "") == 0)
    {
        ESP_LOGW(TAG, "No firmware URL provided. Nothing to flash.");

        httpd_resp_send(req, buffer, strlen(buffer));

        /** Disable Log to Web **/
        if (output_esplog_to_web)
        {
            esp_log_set_putchar(old_logger);
        }

        /** Close HTTP Response **/
        httpd_resp_set_status(req, HTTPD_400);
        httpd_resp_send_chunk(req, NULL, 0);

        return ESP_OK;
    }

    ESP_LOGI(TAG, "Using URL: %s", user_url_buf);

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(req, "", fh_log_buffer, LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    TaskHandle_t xFlashTask = NULL;

    fc.url = user_url_buf;
    fc.req = req;

    BaseType_t xReturned =
        xTaskCreate(&flash_tsk, "flash_tsk", 4096, NULL, 2, &xFlashTask);
    if (xReturned == pdPASS)
    {
        ESP_LOGI(TAG, "Flashing task created.");
    }

    while (1)
    {
        uxBits = xEventGroupWaitBits(flash_event_group,
                                     FLASH_SUCCESS_BIT | FLASH_FAIL_BIT, true,
                                     false, portMAX_DELAY);

        if (uxBits & FLASH_SUCCESS_BIT)
        {
            sprintf(buffer,
                    "Flashed %s successfully, rebooting...\n Sensitive operations occur on first boot and may take up to five minutes.",
                    user_url_buf);
            ESP_LOGI(TAG, buffer);

            /** START Publish to Web UI **/
            tmRet = webui_msgPub(fc.req, "=========== [DONE] ===========", "", LOG_NORM);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            /** START Publish to Web UI **/
            tmRet = webui_msgPub(req, "🎉 Flashing completed successfully.", "Restarting device in 5 seconds.", LOG_SUCCESS);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            /** START Publish to Web UI **/
            tmRet = webui_msgPub(req, "⚠️ Sensitive operations occur on first boot and may take up to five minutes to complete. It is absolutely critical that you do not cut power to the device during this time.", "", LOG_BYE);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            for (int i = 4; i >= 0; i--)
            {
                ESP_LOGI(TAG, "Restarting in %d seconds...", i);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }

            /** Close HTTP Response **/
            ESP_ERROR_CHECK(httpd_resp_send_chunk(req, NULL, 0));

            esp_restart();

            return ESP_OK;
        }

        if (uxBits & FLASH_FAIL_BIT)
        {
            sprintf(buffer, "Flashing %s failed.", user_url_buf);
            ESP_LOGE(TAG, buffer);
            xEventGroupClearBits(flash_event_group, FLASH_FAIL_BIT);

            /** Disable Log to Web **/
            if (output_esplog_to_web)
            {
                esp_log_set_putchar(old_logger);
            }

            /** Close HTTP Response **/
            httpd_resp_send_chunk(req, NULL, 0);

            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

esp_err_t handleUndo(httpd_req_t *req)
{
    memset(buffer, 0, sizeof buffer);

    if (erased_factory_app)
    {
        sprintf(buffer, "Can't undo.");
        ESP_LOGW(TAG, buffer);
    }
    else
    {
        esp_err_t ret = esp_ota_set_boot_partition(part_idle);
        if (ret)
        {
            sprintf(buffer, "Failed to undo.");
            ESP_LOGE(TAG, buffer);
        }
        else
        {
            part_idle = esp_ota_get_next_update_partition(NULL);
            sprintf(buffer, "Undone successfully. Rebooting...");
            ESP_LOGI(TAG, buffer);
            for (int i = 4; i >= 0; i--)
            {
                ESP_LOGI(TAG, "Restarting in %d seconds...", i);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
        }
    }

    httpd_resp_send(req, buffer, strlen(buffer));

    return ESP_OK;
}

esp_err_t handleInfo(httpd_req_t *req)
{
    memset(buffer, 0, sizeof buffer);

    wifi_mode_t ret_wifi_mode;
    char *ret_wifi_mode_human;
    ESP_ERROR_CHECK(esp_wifi_get_mode(&ret_wifi_mode));

    switch (ret_wifi_mode)
    {
    case WIFI_MODE_AP:
        ret_wifi_mode_human = "Acting as Wi-Fi AP (Fallback)";
        break;
    case WIFI_MODE_STA:
        ret_wifi_mode_human = "Conneted to Wi-Fi AP (Smart Config)";
        break;
    default:
        ret_wifi_mode_human = "Other";
    }

    char client_ipstr[40];
    req_get_client_ip(req, client_ipstr);

    const char *FlashSize = "";
    switch (flash_data_addr[3] & 0xF0)
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
    switch (flash_data_addr[2] & 0xF)
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
    switch (flash_data_addr[3] & 0xF)
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

    sprintf(buffer, "%s", (char *)macStr);
    element = createElement("mac", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s", ret_wifi_mode_human);
    element = createElement("wifi_mode", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s", client_ipstr);
    element = createElement("client_ip", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s %s @ %sMHz", FlashSize, FlashMode, FlashSpeed);
    element = createElement("flash_mode", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s @ 0x%06x", part_configured->label,
            part_configured->address);
    element = createElement("part_boot_conf", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s @ 0x%06x", part_running->label, part_running->address);
    element = createElement("part_boot_act", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s @ 0x%06x", part_idle->label, part_idle->address);
    element = createElement("part_idle", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s",
            (erased_factory_app ? "Third party app" : "Factory app"));
    element = createElement("part_idle_content", buffer);
    cJSON_AddItemToArray(elements, element);

    sprintf(buffer, "%s",
            (erased_bootloader ? "Arduino eboot (third party)"
                               : "Espressif (factory)"));
    element = createElement("bootloader", buffer);
    cJSON_AddItemToArray(elements, element);

    element = createElement("progress", "<pre>Ready to flash...</pre>");
    cJSON_AddItemToArray(elements, element);

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

    // Start the httpd server
    // Behind the scenes, starts a task with priority of idle+5
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
 * This is the single s̶p̶a̶g̶h̶e̶t̶t̶i̶ ̶c̶o̶d̶e̶ workhorse function that performs the core write/erase functions
 * related to replacing the device's firmware and bootloader.
 * 
 * This task is started by the http server's handleFlash (/flash) handler.
 *
 * */
static void flash_tsk(void *parm)
{
    static const char *TAG = "ota_task";
    esp_err_t ret;
    esp_err_t tmRet;

    const char *url = fc.url;

    /***
   * INITIALIZE / PRE-CHECK
   * */

    ESP_LOGI(TAG, "Running from %s at 0x%x.\n", part_running->label,
             part_running->address);

    if (part_running->address != part_configured->address)
    {
        // We may want to take action here. For now, just report.
        ESP_LOGW(TAG,
                 "Partition %s at 0x%x configured as boot partition, but currently "
                 "running from %s at 0x%x.",
                 part_configured->label, part_configured->address,
                 part_running->label, part_running->address);
    }

    // Ensure partition addresses meet expectations.
    if (!(((part_running->address == OTA_0_ADDR) ||
           (part_running->address == OTA_1_ADDR)) &&
          ((part_idle->address == OTA_0_ADDR) ||
           (part_idle->address == OTA_1_ADDR)) &&
          (part_idle->address != part_running->address)))
    {
        ESP_LOGE(TAG,
                 "Partition table addresses don't match expectations.\n"
                 "Running %s at 0x%x. Idle partition %s at 0x%x.",
                 part_running->label, part_running->address, part_idle->label,
                 part_idle->address);

        sprintf(df_log_buffer,
                "Partition table addresses don't match expectations. "
                "You should revert to factory firmware. "
                "(Running %s at 0x%x. Idle partition %s at 0x%x.)",
                part_running->label, part_running->address, part_idle->label,
                part_idle->address);

        /** START Publish to Web UI **/
        sprintf(df_log_buffer, "Fetching firmware from %s.", url);
        tmRet = webui_msgPub(fc.req, "[Pre-Check Failure]", df_log_buffer, LOG_NORM);
        ESP_LOGD(TAG, (char *)tmRet);
        /** END Publish to Web UI **/

        xEventGroupSetBits(flash_event_group, FLASH_FAIL_BIT);
        vTaskDelete(NULL);
    }

    /***
   * DOWNLOAD & FLASH APP
   * */

    ESP_LOGI(TAG, "BEGIN: Downloading and flashing third party bin.");

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "======== [Task 1 of 5] ========", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "⏳ Downloading and flashing firmware.", "This may take a few minutes and you won't see any signs of progress. Be patient...", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    esp_http_client_config_t config = {.url = fc.url,
                                       .event_handler = _http_event_handler};

    int attempt = 0;

    while (attempt <= 14)
    {
        // TO-DO: UNBLOCK THIS
        ret = esp_https_ota(&config);

        if (ret == ESP_ERR_HTTP_CONNECT && attempt < 14)
        {
            ESP_LOGW(TAG, "HTTP connection failed. Trying again...");

            /** START Publish to Web UI **/
            sprintf(df_log_buffer,
                    "Trying again in 15 seconds."
                    "(Attempt %d of 15.)",
                    attempt + 1);
            tmRet = webui_msgPub(fc.req, "HTTP Connection failed.", df_log_buffer, LOG_WARN);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            printf("Retrying in:\n");
            for (int i = 14; i >= 0; i--)
            {
                printf("%d ", i + 1);
                fflush(stdout);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            printf("\n");
        }
        else if (ret == ESP_OK)
        {
            erased_factory_app = true;

            /** START Publish to Web UI **/
            tmRet = webui_msgPub(fc.req, "✅ Successfully downloaded and installed new firmware.", "", LOG_SUCCESS);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            break;
        }
        else
        {
            /** START Publish to Web UI **/
            tmRet = webui_msgPub(fc.req, "", "Failed while installing firmware. "
                                             "This should be recoverable. Try again after verifying that your "
                                             "internet connection is working and that your firmware URL is "
                                             "correct. ",
                                 LOG_FAIL);
            ESP_LOGD(TAG, (char *)tmRet);
            tmRet = webui_msgPub(fc.req, "*** DO NOT REVERT TO FACTORY FIRMWARE!. ***",
                                 "Factory firmware was partially overwritten during this flashing attempt. "
                                 "Reverting to this overwritten firmware will brick your device.",
                                 LOG_FAIL);
            ESP_LOGD(TAG, (char *)tmRet);
            /** END Publish to Web UI **/

            erased_factory_app = true;
            xEventGroupSetBits(flash_event_group, FLASH_FAIL_BIT);
            vTaskDelete(NULL);
        }
    }

    ESP_LOGI(TAG, "DONE: Downloading and flashing third party bin.");

    /***
   * ERASE FLASH - BOOTLOADER
   *
   * Point of No Return. If there's an error here, we're screwed.
   * */

    ESP_LOGI(TAG, "BEGIN: Erasing bootloader.");

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "======== [Task 2 of 5] ========", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "⏳ Erasing factory bootloader...", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    // Failing here will PROBABLY brick the device.
    taskENTER_CRITICAL();
    ret = spi_flash_erase_range(0x0, 0x10000);
    taskEXIT_CRITICAL();
    if (ret != ESP_OK)
    {
        erased_bootloader = true;

        /** START Publish to Web UI **/
        tmRet = webui_msgPub(fc.req, "", "Failed while erasing bootloader. Your device is probably "
                                         "bricked 😬. Sorry!",
                             LOG_FAIL);
        ESP_LOGD(TAG, (char *)tmRet);
        /** END Publish to Web UI **/

        xEventGroupSetBits(flash_event_group, FLASH_FAIL_BIT);
        vTaskDelete(NULL);
    }

    erased_bootloader = true;

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "✅ Successfully erased factory bootloader.", "", LOG_SUCCESS);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    ESP_LOGI(TAG, "DONE: Erasing bootloader.");

    /***
   * WRITE BOOTLOADER
   * */

    ESP_LOGI(TAG, "BEGIN: Writing bootloader.");

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "======== [Task 3 of 5] ========", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "⏳ Writing Arduino bootloader...", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/
    unsigned char *eboot_bin_ptr = eboot_bin;

    // Preserve existing flash settings in bootloader
    eboot_bin_ptr[2] = flash_data_addr[2];
    eboot_bin_ptr[3] = flash_data_addr[3];

    taskENTER_CRITICAL();
    ret = spi_flash_write(0x0, (uint32_t *)eboot_bin_ptr, eboot_bin_len);
    taskEXIT_CRITICAL();
    if (ret != ESP_OK)
    {
        /** START Publish to Web UI **/
        tmRet = webui_msgPub(fc.req, "", "Failed while writing Arduino bootloader. Your device is "
                                         "probably bricked 😬. Sorry!",
                             LOG_FAIL);
        ESP_LOGD(TAG, (char *)tmRet);
        /** END Publish to Web UI **/
        xEventGroupSetBits(flash_event_group, FLASH_FAIL_BIT);
        vTaskDelete(NULL);
    }

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "✅ Successfully wrote Arduino bootloader.", "", LOG_SUCCESS);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    ESP_LOGI(TAG, "DONE: Writing bootloader.");

    ESP_LOGI(TAG, "BEGIN: Setting eboot commands.");

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "======== [Task 4 of 5] ========", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "⏳ Saving initialization instructions for Arduino bootloader...", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    // Command the bootloader to copy the firmware to the correct place on next
    // boot
    struct eboot_command ebcmd;
    ebcmd.action = ACTION_COPY_RAW;
    ebcmd.args[0] = part_idle->address;
    ebcmd.args[1] = 0x0;
    ebcmd.args[2] = part_idle->size;

    taskENTER_CRITICAL();
    eboot_command_write(&ebcmd);
    taskEXIT_CRITICAL();

    struct eboot_command ebcmd_read;
    eboot_command_read(&ebcmd_read);
    ESP_LOGD(TAG, "Action:          %i", ebcmd_read.action);
    ESP_LOGD(TAG, "Source Addr:     0x%x", ebcmd_read.args[0]);
    ESP_LOGD(TAG, "Dest Addr:       0x%x", ebcmd_read.args[1]);
    ESP_LOGD(TAG, "Size:            0x%x", ebcmd_read.args[2]);

    ESP_LOGI(TAG, "DONE: Setting eboot commands.");

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "✅ Successfully saved Arduino bootloader initialization instructions.", "", LOG_SUCCESS);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /***
   * ERASE FLASH - RF CALIBRATION DATA
   * */

    /**
   * Erase rf calibration data. Needed to obviate Reset 5 commant in Tasmota to
   * prevent frequent reboots. This doesn't seem to work for the time being.
   * Reset 5 still needed.
   * */

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "======== [Task 5 of 5] ========", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    /** START Publish to Web UI **/
    tmRet = webui_msgPub(fc.req, "⏳ Performing housekeeping...", "", LOG_NORM);
    ESP_LOGD(TAG, (char *)tmRet);
    /** END Publish to Web UI **/

    ESP_LOGI(TAG, "BEGIN: Erasing RF calibration data.");
    uint32_t rf_cal_sector = user_rf_cal_sector_set();

    if (!rf_cal_sector)
    {
        /**
     * If we've made it this far, we've succeeded in loading the new firmware,
     * so don't hard fail on error.
     *
     * If we can't clear RF cal data, Tasmota may experience constant
     * Wi-Fi disconnects. This can be fixed with Tasmota console
     * command `Reset 5` followed by a hard power cycle
     * (unplug, then plug back in).
     * */
        ESP_LOGE(TAG, "Failed to get RF calibration data.");
        /** START Publish to Web UI **/
        tmRet = webui_msgPub(fc.req, "", "Hosekeeping failed. Don't worry, this isn't a big deal.", LOG_WARN);
        ESP_LOGD(TAG, (char *)tmRet);
        /** END Publish to Web UI **/
    }
    else
    {
        ESP_LOGI(TAG, "RF calibration sector at 0x%06x.", rf_cal_sector);

        taskENTER_CRITICAL();
        ret = spi_flash_erase_sector(rf_cal_sector);
        taskEXIT_CRITICAL();

        ESP_LOGI(TAG, "DONE: Erasing RF calibration data.");
        /** START Publish to Web UI **/
        tmRet = webui_msgPub(fc.req, "✅ Successfully kept house.", "", LOG_SUCCESS);
        ESP_LOGD(TAG, (char *)tmRet);
        /** END Publish to Web UI **/
    }

    xEventGroupSetBits(flash_event_group, FLASH_SUCCESS_BIT);

    vTaskDelete(NULL);
}

/**
 * 
 * HELPER FUNCTIONS
 * 
 */

uint32_t user_rf_cal_sector_set(void)
{
    flash_size_map size_map = system_get_flash_size_map();
    uint32_t rf_cal_sec = 0;

    switch (size_map)
    {
    case FLASH_SIZE_4M_MAP_256_256:
        rf_cal_sec = 128 - 5;
        break;

    case FLASH_SIZE_8M_MAP_512_512:
        rf_cal_sec = 256 - 5;
        break;

    case FLASH_SIZE_16M_MAP_512_512:
    case FLASH_SIZE_16M_MAP_1024_1024:
        rf_cal_sec = 512 - 5;
        break;

    case FLASH_SIZE_32M_MAP_512_512:
    case FLASH_SIZE_32M_MAP_1024_1024:
        rf_cal_sec = 1024 - 5;
        break;

    default:
        rf_cal_sec = 0;
        break;
    }

    return rf_cal_sec;
}

int req_get_client_ip(httpd_req_t *req, char ipstr[40])
{
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_in addr;
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0)
    {
        ESP_LOGE(TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    // Convert to IPv4 string
    inet_ntop(AF_INET, &addr.sin_addr, ipstr, 40);
    ESP_LOGI(TAG, "Client IP => %s", ipstr);

    return ESP_OK;
}

cJSON *createElement(char *id, char *innerHTML)
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

void app_main()
{
    static const char *TAG = "main";

    // Use 115200 for compatibility with gdb.
    ESP_ERROR_CHECK(uart_set_baudrate(0, 115200));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Retrieve flash headers
    ESP_LOGI(TAG, "BEGIN: Retrieving system data.");

    part_running = esp_ota_get_running_partition();
    part_configured = esp_ota_get_boot_partition();
    part_idle = esp_ota_get_next_update_partition(NULL);

    spi_flash_read(0x0, (uint32_t *)flash_data_addr, 4);
    ESP_LOGI(TAG,
             "Magic: %02x, Segments: %02x, Flash Mode: %02x, Size/Freq: %02x",
             flash_data_addr[0], flash_data_addr[1], flash_data_addr[2],
             flash_data_addr[3]);

    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));

    snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);

    real_flash_size = spi_flash_get_chip_size();

    ESP_LOGI(TAG, "DONE: Retrieving system data.");

    ESP_LOGI(TAG, "BEGIN: Start AP and web server.");

    ESP_LOGI(TAG, "Starting easy flash.");

    if (DEVMODE)
    {
        wifi_init_sta();
    }
    else
    {
        smartConfig_init();
    }

    // Start Webserver

    sprintf(http_last_modified, "%s %s GMT", __DATE__, __TIME__);

    if (server == NULL)
    {
        ESP_LOGI(TAG, "Starting webserver");
        server = start_webserver();
    }

    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
    // WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    ESP_LOGI(TAG, "DONE: Start AP and web server.");

    // ESP_LOGI(TAG, "Starting task-to-web message queue.");
    // taskMessages = xQueueCreate(10, sizeof(taskMessage));

    ESP_LOGI(TAG, "Initialization complete. Waiting for user command.");

    strcpy(status_txt, "Ready to flash.");
}