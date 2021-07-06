/*
These features are not currently in use.
*/

#if FEAT_DEBUG_LOG_TO_WEBUI
static putchar_like_t old_logger = NULL;
static char debug_logBuffer[LOG_BUF_MAX_LINE_SIZE];
#endif

#if FEAT_DEBUG_LOG_TO_WEBUI
static httpd_req_t *flash_httpdReq;
#endif

#include "esp2ino_common.h"

#if FEAT_DEBUG_LOG_TO_WEBUI
static int webui_debugPub(int chr);
static cJSON *createDebugStatusElement(char *msg);
#endif

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
    // char *jsonBuffer;
    // static const char *TAG = "webui_debugPub";
    size_t len = strlen(debug_logBuffer);
    if (chr == '\n')
    {
        // webMsgQueueSize += 1;

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
        // webMsgQueueSize -= 1;
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

#if FEAT_DEBUG_LOG_TO_WEBUI
static cJSON *createDebugStatusElement(char *msg)
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
#endif