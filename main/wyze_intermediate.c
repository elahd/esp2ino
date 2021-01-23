#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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
#include "nvs.h"
#include "nvs_flash.h"

#include "eboot_bin.h"
#include "eboot_command.h"

#include "wyze_intermediate.h"

#define WIFI_SSID  "wyze_plug_flasher"
#define WIFI_MAX_CON    10

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

static const char *default_ota_url_proto = "http";
static const char *default_ota_url_port = "8080";
static const char *default_ota_url_filepath = "thirdparty.bin";

const esp_partition_t *part_running;
const esp_partition_t *part_configured;
const esp_partition_t *part_idle;

uint32_t flash_data;
uint8_t *flash_data_addr = (uint8_t *)&flash_data;
// const unsigned int * real_flash_size_ptr;
static unsigned int real_flash_size;

char buffer[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4))) = {0};

static httpd_handle_t server = NULL;
uint8_t mac[6];
char macStr[18];

bool erased_bootloader = false;
bool erased_factory_app = false;

static const char *TAG = "global";

/***
 * WIFI/HTTP REQUEST FUNCTIONS
 * */

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {

  static const char *TAG = "http_event";

  switch (evt->event_id) {
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  static const char *TAG = "wifi_event";

  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
             event->aid);
  }
}

void wifi_init_softap() {
  static const char *TAG = "wifi_init";

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

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));

  wifi_config_t wifi_config = {
      .ap = {.ssid = WIFI_SSID,
             .ssid_len = strlen(WIFI_SSID),
             .max_connection = WIFI_MAX_CON,
             .authmode = WIFI_AUTH_OPEN}
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s", WIFI_SSID);
}

/***
 * HTTP SERVER FUNCTIONS
 * */

httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = handleRoot};

httpd_uri_t flash = {
    .uri = "/flash", .method = HTTP_GET, .handler = handleFlash};

httpd_uri_t backup = {
    .uri = "/backup", .method = HTTP_GET, .handler = handleBackup};

httpd_uri_t undo = {.uri = "/undo", .method = HTTP_GET, .handler = handleUndo};

int req_get_client_ip(httpd_req_t *req, char ipstr[40]) {
  int sockfd = httpd_req_to_sockfd(req);
  struct sockaddr_in addr;
  socklen_t addr_size = sizeof(addr);

  if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0) {
    ESP_LOGE(TAG, "Error getting client IP");
    return FAIL_INIT;
  }

  // Convert to IPv4 string
  inet_ntop(AF_INET, &addr.sin_addr, ipstr, 40);
  ESP_LOGI(TAG, "Client IP => %s", ipstr);

  return ESP_OK;
}

char build_ota_url(char *ipstr, char *ota_full) {
  if (ipstr == NULL || default_ota_url_proto == NULL ||
      default_ota_url_port == NULL || default_ota_url_filepath == NULL) {
    return FAIL;
  }

  char url_buf[150];
  sprintf(url_buf, "%s://%s:%s/%s", default_ota_url_proto, ipstr,
          default_ota_url_port, default_ota_url_filepath);

  if (strcpy(ota_full, url_buf)) {
    return ESP_OK;
  }

  return FAIL;
}

esp_err_t handleRoot(httpd_req_t *req) {
  memset(buffer, 0, sizeof buffer);

  char *ip = "10.0.0.1";

  char client_ipstr[40];
  req_get_client_ip(req, client_ipstr);

  char full_ota_url[150];
  build_ota_url(client_ipstr, full_ota_url);

  // BROKEN: Flash size not displaying. Fix later.
  const char *FlashSize = "";
  switch (flash_data_addr[3] & 0xF0) {
  case 0x0:
    FlashSize = "512K";
    break;
  case 0x1:
    FlashSize = "256K";
    break;
  case 0x2:
    FlashSize = "1M";
    break;
  case 0x3:
    FlashSize = "2M";
    break;
  case 0x4:
    FlashSize = "4M";
    break;
  case 0x8:
    FlashSize = "8M";
    break;
  case 0x9:
    FlashSize = "16M";
    break;
  }

  const char *FlashMode = "";
  switch (flash_data_addr[2] & 0xF) {
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
  switch (flash_data_addr[3] & 0xF) {
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

  sprintf(
      buffer,
      "<h1><i><u>WYZE PLUG FLASHER</u></i></h1>\n"
      // BROKEN: Backup function not working. Hiding link.
      //"Download Backup: <a href='http://%s/backup'>http://%s/backup</a>"
      "<br><br>\n"
      "Flash Firmware: <a "
      "href='http://%s/flash?url=%s'>http://%s/flash?url=%s</a>"
      "<br><br>\n"
      "Revert to Factory Firmware: <a href='http://%s/undo'>http://%s/undo</a>"
      "<br><br>\n"
      "<hr>\n"
      "<b>Your IP: %s"
      "\n"
      "<hr>\n"
      "<b>MAC:</b> %s"
      "<br>\n"
      "<b>FlashMode:</b> %s %s @ %sMHz"
      "<br>\n"
      "<b>Configured Boot Partition:</b> %s @ 0x%06x"
      "<br>\n"
      "<b>Actual Boot Partition:</b> %s @ 0x%06x"
      "<br>\n"
      "<b>Idle Partition:</b> %s @ 0x%06x"
      "<br>\n"
      "<b>Idle Partition Content:</b> %s"
      "<br>\n"
      "<b>Bootloader:</b> %s"
      "<br>\n",
      /* ip, ip, */
      ip, full_ota_url, ip, full_ota_url, ip, ip, client_ipstr,
      (char *)macStr, FlashSize, FlashMode, FlashSpeed, part_configured->label,
      part_configured->address, part_running->label, part_running->address,
      part_idle->label, part_idle->address,
      (erased_factory_app ? "Third party app" : "Factory app"),
      (erased_bootloader ? "Arduino eboot (third party)"
                         : "Espressif (factory)"));

  httpd_resp_send(req, buffer, strlen(buffer));

  return ESP_OK;
}

/**
 * BROKEN: handleBackup is not working. Returns .bin with MD5 mismatch. May be caused by
 * spi_flash_get_chip_size() not returning actual flash size. This function
 * retrieves flash size from image headers, not from SPI chip itself.
 * */
esp_err_t handleBackup(httpd_req_t *req) {
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
  while (read < real_flash_size) {
    spi_flash_read(read, (char *)buf, OTA_BUF_SIZE);
    httpd_resp_send_chunk(req, buf, OTA_BUF_SIZE);
    read += OTA_BUF_SIZE;
  }

  httpd_resp_send_chunk(req, NULL, 0);

  ESP_LOGI(TAG, "DONE: Sending backup.");

  return ESP_OK;
}

esp_err_t handleFlash(httpd_req_t *req) {

  char *buf;
  size_t buf_len;
  char *user_url_buf = {0};

  char client_ipstr[40];
  req_get_client_ip(req, client_ipstr);

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      ESP_LOGI(TAG, "Found URL query => %s", buf);
      // Get URL parameter
      if (httpd_query_key_value(buf, "url", user_url_buf,
                                sizeof(*user_url_buf)) == ESP_OK) {
        ESP_LOGI(TAG, "Found URL query parameter => url=%s", user_url_buf);
      }
    }
    free(buf);
  }

  char full_ota_url[150];
  build_ota_url(client_ipstr, full_ota_url);
  const char *target_url = user_url_buf ? user_url_buf : full_ota_url;

  ESP_LOGI(TAG, "Using URL: %s", target_url);

  esp_err_t ret = do_flash(target_url);

  if (!ret) {
    sprintf(buffer,
            "Flashed %s successfully, rebooting...<br>\n<b>Sensitive "
            "operations occur on first boot and may take up to five minutes.",
            target_url);
    ESP_LOGI(TAG, buffer);
    httpd_resp_send(req, buffer, strlen(buffer));

    for (int i = 4; i >= 0; i--) {
      ESP_LOGI(TAG, "Restarting in %d seconds...", i);
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    esp_restart();
  }

  sprintf(buffer, "Flashing %s failed, 0x%02x.", target_url, ret);
  ESP_LOGE(TAG, buffer);
  httpd_resp_send(req, buffer, strlen(buffer));
  return ESP_OK;
}

esp_err_t handleUndo(httpd_req_t *req) {
  memset(buffer, 0, sizeof buffer);

  if (erased_factory_app) {
    sprintf(buffer, "Can't undo.");
    ESP_LOGW(TAG, buffer);
  } else {
    esp_err_t ret = esp_ota_set_boot_partition(part_idle);
    if (ret) {
      sprintf(buffer, "Failed to undo.");
      ESP_LOGE(TAG, buffer);
    } else {
      part_idle = esp_ota_get_next_update_partition(NULL);
      sprintf(buffer, "Undone successfully. Rebooting...");
      ESP_LOGI(TAG, buffer);
      for (int i = 4; i >= 0; i--) {
        ESP_LOGI(TAG, "Restarting in %d seconds...", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
      }
    }
  }

  httpd_resp_send(req, buffer, strlen(buffer));

  return ESP_OK;
}

void stop_webserver(httpd_handle_t server) {
  // Stop the httpd server
  httpd_stop(server);
}

httpd_handle_t start_webserver(void) {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();

  // Start the httpd server
  ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &flash);
    httpd_register_uri_handler(server, &undo);
    httpd_register_uri_handler(server, &backup);
    return server;
  }

  ESP_LOGI(TAG, "Error starting server!");
  return NULL;
}

static void disconnect_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  httpd_handle_t *server = (httpd_handle_t *)arg;
  if (*server) {
    ESP_LOGI(TAG, "Stopping webserver");
    stop_webserver(*server);
    *server = NULL;
  }
}

/***
 * CORE FUNCTIONS
 * */

static int do_flash(const char *url) {
  static const char *TAG = "ota_task";
  esp_err_t ret;

  /***
   * INITIALIZE / PRE-CHECK
   * */

  ESP_LOGI(TAG, "Running from %s at 0x%x.\n", part_running->label,
           part_running->address);

  if (part_running->address != part_configured->address) {
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
        (part_idle->address != part_running->address))) {
    ESP_LOGE(TAG,
             "Partition table addresses don't match expectations.\n"
             "Running %s at 0x%x. Idle partition %s at 0x%x.",
             part_running->label, part_running->address, part_idle->label,
             part_idle->address);

    return FAIL_INIT;
  }

  /***
   * DOWNLOAD & FLASH APP
   * */

  ESP_LOGI(TAG, "BEGIN: Downloading and flashing third party bin.");

  esp_http_client_config_t config = {.url = url,
                                     .event_handler = _http_event_handler};

  int attempt = 0;

  while (attempt <= 14) {
    ret = esp_https_ota(&config);
    if (ret == ESP_ERR_HTTP_CONNECT && attempt < 14) {
      printf("Retrying in:\n");
      for (int i = 14; i >= 0; i--) {
        printf("%d ", i + 1);
        fflush(stdout);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
      }
      printf("\n");
    } else if (ret == ESP_OK) {
      erased_factory_app = true;
      break;
    } else {
      erased_factory_app = true;
      return FAIL_APP_OTA;
    }
  }

  ESP_LOGI(TAG, "DONE: Downloading and flashing third party bin.");

  /***
   * ERASE FLASH - BOOTLOADER
   *
   * Point of No Return. If there's an error here, we're screwed.
   * */

  ESP_LOGI(TAG, "BEGIN: Erasing bootloader.");

  // Failing here will PROBABLY brick the device, but we should try to recover,
  // anyway.
  ret = spi_flash_erase_range(0x0, 0x10000);
  if (ret != ESP_OK) {
    erased_bootloader = true;
    return FAIL_BOOT_ERASE;
  }

  erased_bootloader = true;

  ESP_LOGI(TAG, "DONE: Erasing bootloader.");

  /***
   * WRITE BOOTLOADER
   * */

  ESP_LOGI(TAG, "BEGIN: Writing bootloader.");

  unsigned char *eboot_bin_ptr = eboot_bin;

  // Preserve existing flash settings in bootloader
  eboot_bin_ptr[2] = flash_data_addr[2];
  eboot_bin_ptr[3] = flash_data_addr[3];

  ret = spi_flash_write(0x0, (uint32_t *)eboot_bin_ptr, eboot_bin_len);
  if (ret != ESP_OK) {
    return FAIL_BOOT_WRITE;
  }

  ESP_LOGI(TAG, "DONE: Writing bootloader.");

  ESP_LOGI(TAG, "BEGIN: Setting eboot commands.");
  // Command the bootloader to copy the firmware to the correct place on next boot
  struct eboot_command ebcmd;
  ebcmd.action = ACTION_COPY_RAW;
  ebcmd.args[0] = part_idle->address;
  ebcmd.args[1] = 0x0;
  ebcmd.args[2] = part_idle->size;
  eboot_command_write(&ebcmd);

  struct eboot_command ebcmd_read;
  eboot_command_read(&ebcmd_read);
  ESP_LOGD(TAG, "Action:          %i", ebcmd_read.action);
  ESP_LOGD(TAG, "Source Addr:     0x%x", ebcmd_read.args[0]);
  ESP_LOGD(TAG, "Dest Addr:       0x%x", ebcmd_read.args[1]);
  ESP_LOGD(TAG, "Size:            0x%x", ebcmd_read.args[2]);

  ESP_LOGI(TAG, "DONE: Setting eboot commands.");

  /***
   * ERASE FLASH - RF CALIBRATION DATA
   * */

  /** 
   * BROKEN: Fails on user_rf_cal_sector_set. Disabling for now.
   * Workaround outlined in README.
   * */

  // ESP_LOGI(TAG, "BEGIN: Erasing RF calibration data.");
  // uint32_t rf_cal_sector = user_rf_cal_sector_set();

  // if (!rf_cal_sector) {
  //   /**
  //    * If we've made it this far, we're functionally successful,
  //    * so don't hard fail on error.
  //    *
  //    * If we can't clear RF cal data, Tasmota may experience constant
  //    * WiFi disconnects. This can be fixed with Tasmota console
  //    * command `Reset 3` followed by a hard power cycle
  //    * (unplug, then plug back in).
  //    * */
  //   ESP_LOGE(TAG, "Failed to get RF calibration data.");
  // } else {
  //   ESP_LOGI(TAG, "RF calibration sector at 0x%06x.", rf_cal_sector);
  //   ret = spi_flash_erase_sector(rf_cal_sector);
  //   ESP_LOGI(TAG, "DONE: Erasing RF calibration data.");
  // }

  return SUCCESS;
}

uint32_t user_rf_cal_sector_set(void) {
  flash_size_map size_map = system_get_flash_size_map();
  uint32_t rf_cal_sec = 0;

  switch (size_map) {
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

void app_main() {
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

  snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);

  real_flash_size = spi_flash_get_chip_size();

  ESP_LOGI(TAG, "DONE: Retrieving system data.");

  ESP_LOGI(TAG, "BEGIN: Start AP and web server.");

  ESP_LOGI(TAG, "Starting AP");
  wifi_init_softap();

  // Start Webserver
  if (server == NULL) {
    ESP_LOGI(TAG, "Starting webserver");
    server = start_webserver();
  }

  ESP_ERROR_CHECK(esp_event_handler_register(
      WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

  ESP_LOGI(TAG, "DONE: Start AP and web server.");

  ESP_LOGI(TAG, "Initialization complete. Waiting for user command.");
}