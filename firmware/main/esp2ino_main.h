#pragma once

#include <http_parser.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "cJSON.h"
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
#include "esp_smartconfig.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "smartconfig_ack.h"

/***
 * FUNCTIONS
 * */

/** esp2ino_main.c **/
esp_err_t _http_event_handler(esp_http_client_event_t *evt);
esp_err_t handleRoot(httpd_req_t *req);
esp_err_t handleFlash(httpd_req_t *req);
esp_err_t handleBackup(httpd_req_t *req);
esp_err_t handleInfo(httpd_req_t *req);
esp_err_t handleUndo(httpd_req_t *req);
void stop_webserver(httpd_handle_t server);
httpd_handle_t start_webserver(void);
void app_main();
uint32_t user_rf_cal_sector_set(void);
cJSON *createElement(char *id, char *innerHTML);
int req_get_client_ip(httpd_req_t *req, char ipstr[40]);

/** esp2ino_flash.c **/
static void flash_tsk(void *parm);

/** esp2ino_wifi.c **/
static void wifiFallback_eventHandler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data);
static void wifiFallback_init(xTimerHandle pxTimer);
static void smartConfig_init(void);
static void smartConfig_taskMgr(void *parm);
static void smartConfig_eventHandler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);

/** esp2ino_log.c **/
static esp_err_t webui_msgPub(httpd_req_t *req, char *intro, char *message,
                              int msgType);
int webui_debugPub(int chr);

/** wifi_debug.c **/
void wifi_init_sta(void);