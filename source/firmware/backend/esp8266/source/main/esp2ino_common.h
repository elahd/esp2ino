#ifndef esp2ino_common
#define esp2ino_common

#include <http_parser.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "urlsafe.h"
#include "esp_private/esp_system_internal.h"

#include "eboot_command.h"
#include "eboot_bin.h"
#include "index.html.gz.h"
#include "esp2ino_settings.h"

/** Macros **/

/**
 * Pre-Processor Directives & Variables
 * */

#define OTA_TYPE_UPLOAD 0
#define OTA_TYPE_DOWNLOAD 1

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
#define STA_WIFI_SCAN_RESULTS_BIT BIT2
#define STA_WIFI_NUM_ATTEMPTS 5
#define AP_MODE_SSID "esp2ino"
#define AP_MODE_MAX_CON 10

/** Misc **/
#define BACKUP_BUF_SIZE 250
#define LOG_BUF_MAX_LINE_SIZE 250
#define OTA_UPLD_BUF_SIZE 512
#define REBOOT_NOW_BIT BIT3

/** Functions **/
#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

#ifndef MAX
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#endif

/***
 * 
 * Global Functions & Variables
 * 
 * TO-DO: Replace globals with getters & setters.
 * 
 * */

/**
 * esp2ino_main.c
 * **/
extern void app_main();
extern esp_err_t flash_viaUpload(httpd_req_t *req);

extern TaskHandle_t xRebootAgent;
extern EventGroupHandle_t rebootAgent_EventGroup;
extern ip4_addr_t wifi__ip_ap;

/** System Attributes **/
extern esp_chip_info_t sys_chipInfo;
extern char sys_chipInfoFeatures_s[50];
extern char *sys_chipInfoModel_s;
extern char sys_chipInfoCores_s[8];
extern char sys_chipInfoRev_s[8];
extern const esp_partition_t *sys_partRunning;
extern const esp_partition_t *sys_partConfigured;
extern const esp_partition_t *sys_partIdle;
extern char sys_hostname[25];

/** Flashing **/
extern uint8_t flash_data[4];
extern bool flash_erasedBootloader;
extern bool flash_erasedFactoryApp;

/**
 * esp2ino_flash.c
 * **/
esp_err_t doFlash(httpd_req_t *req, int otaType);

/**
 * esp2ino_server.c
 * **/
esp_err_t webui_msgPub(httpd_req_t *req, char *step, char *state, char *message);
httpd_handle_t server__start(void);
void server__stop(httpd_handle_t server);
esp_err_t handleUndo(httpd_req_t *req);

extern unsigned int sys_realFlashSize;
extern uint8_t sys_mac[6];
extern char sys_mac_str[18];
extern char sys_statusMsg[100];
extern char user_url_buf[200];
extern char httpRespBuffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4)));
extern esp_err_t flashSuccessWrapUp(httpd_req_t *req);

/**
 * esp2ino_wifi.c
 * **/
bool wifi__apSta_init(void);
esp_err_t wifi__apStaSetHelper(httpd_req_t *req, char param_ssid[32], char param_pswd[64]);
esp_err_t wifi__apStaScanHelper(httpd_req_t *req, char *respBuffer, size_t respBufferSize);

extern char wifi__staSsid[32];
extern char wifi__staBssid[6];
extern char wifi__ip_sta_str[INET_ADDRSTRLEN];
extern char wifi__ip_ap_str[INET_ADDRSTRLEN];
extern char *wifi__mode;
extern bool wifi__isConnected;
extern char wifi__channel[6];
extern ip4_addr_t server_ip;
extern SemaphoreHandle_t wifi__connectSemaphore;

/**
 * esp2ino_safemode.c
 * **/
bool safemode__wifi_init(void);
httpd_handle_t safemode__server_start(void);
void safemode__server_stop(httpd_handle_t server);
esp_err_t safemode__legacy_event_handler(void *ctx, system_event_t *event);

bool safemode__enabled;

#endif //"esp2ino_common"