// --- Required includes for RTOS and synchronization ---
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2c_slave.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
// #include "mdns.h"
#include "lwip/ip4_addr.h"

/*
i2c_slave_v2.c has been modified to disable clock stretching.
This is necessary to prevent the I2C slave from blocking the master when it is not ready
*/

#define I2C_SLAVE_SCL_IO (gpio_num_t)9
#define I2C_SLAVE_SDA_IO (gpio_num_t)8
#define I2C_SLAVE_NUM 0
#define ESP_SLAVE_ADDR 0x22

#define WIFI_SSID_MAXLEN 31 // max SSID length is 31 characters
#define WIFI_PASS_MAXLEN 63 // max password length is 63 characters
#define MAX_SSE_CLIENTS 4   // Maximum number of SSE clients

#define CMD_WIFI_UID 0x30
#define CMD_WIFI_SSID 0x31
#define CMD_WIFI_PASS 0x32
#define CMD_WIFI_START 0x33

#define CMD_SENSOR_READ 0x40

#define CMD_SEND_AVAIL 0x50
#define CMD_SEND_BUFFER 0x51

typedef struct
{
    i2c_slave_dev_handle_t slave_handle;
    QueueHandle_t event_queue;
    QueueHandle_t cmd_queue; // FreeRTOS queue for received command buffers
    char wifi_ssid[WIFI_SSID_MAXLEN + 1];
    char wifi_pass[WIFI_PASS_MAXLEN + 1];
    uint16_t web_uid;                // unique id for mDNS
    uint8_t ret_cmd;                 // bit pattern to indicate supposed state of switches; light, fan, door
    bool wifi_started;               // flag to indicate if WiFi is connected
    SemaphoreHandle_t ret_cmd_mutex; // mutex for ret_cmd
    httpd_handle_t http_server;      // HTTP server handle for SSE
    float sensor_data[8];            // latest sensor data (up to 8 sensors)
} i2c_slave_context_t;

// ===== WIFI Task =====

i2c_slave_context_t context = {
    .slave_handle = NULL,
    .event_queue = NULL,
    .cmd_queue = NULL,
    .wifi_ssid = "",
    .wifi_pass = "",
    .web_uid = 69,
    .ret_cmd = 0b00000000,
    .wifi_started = false,
    .ret_cmd_mutex = NULL,
    .http_server = NULL,
    .sensor_data = {0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f,0.0f}
};
/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "wifi station";
static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < 20) // Retry up to 20 times
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, context.wifi_ssid, WIFI_SSID_MAXLEN);
    strlcpy((char *)wifi_config.sta.password, context.wifi_pass, WIFI_PASS_MAXLEN);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap");
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect");
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// ===== SPIFFS File Server =====
// ===== Set Command Handler =====
static esp_err_t set_cmd_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP", "Received set_cmd request");
    char buf[32];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    int new_cmd = -1;
    sscanf(buf, "ret_cmd=%d", &new_cmd);
    if (new_cmd < 0 || new_cmd > 7)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid value");
        return ESP_FAIL;
    }
    xSemaphoreTake(context.ret_cmd_mutex, portMAX_DELAY);
    context.ret_cmd = (uint8_t)new_cmd;
    xSemaphoreGive(context.ret_cmd_mutex);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ===== Status GET Handler =====
static esp_err_t status_handler(httpd_req_t *req)
{
    ESP_LOGI("HTTP", "Received status request");
    
    // Get current status
    uint8_t current_ret_cmd;
    float sensor_snapshot[8];
    xSemaphoreTake(context.ret_cmd_mutex, portMAX_DELAY);
    current_ret_cmd = context.ret_cmd;
    memcpy(sensor_snapshot, context.sensor_data, sizeof(sensor_snapshot));
    xSemaphoreGive(context.ret_cmd_mutex);

    // Compose JSON response with ret_cmd and sensor_data
    char response[256];
    int len = snprintf(response, sizeof(response), "{\"ret_cmd\":%d,\"sensor_data\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]}",
        current_ret_cmd,
        sensor_snapshot[0], sensor_snapshot[1], sensor_snapshot[2], sensor_snapshot[3],
        sensor_snapshot[4], sensor_snapshot[5], sensor_snapshot[6], sensor_snapshot[7]);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static const char *get_mime_type(const char *filename)
{
    if (strstr(filename, ".html"))
        return "text/html";
    if (strstr(filename, ".css"))
        return "text/css";
    if (strstr(filename, ".js"))
        return "application/javascript";
    if (strstr(filename, ".json"))
        return "application/json";
    if (strstr(filename, ".png"))
        return "image/png";
    if (strstr(filename, ".jpg") || strstr(filename, ".jpeg"))
        return "image/jpeg";
    return "text/plain";
}

static esp_err_t file_handler(httpd_req_t *req)
{
    char filepath[64];
    // Handle root path
    if (strcmp(req->uri, "/") == 0)
    {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    }
    else
    {
        // Limit the length of req->uri to avoid buffer overflow
        snprintf(filepath, sizeof(filepath), "/spiffs%.*s", (int)(sizeof(filepath) - 8 - 1), req->uri);
    }

    FILE *file = fopen(filepath, "r");
    if (!file)
    {
        ESP_LOGE("HTTP", "Failed to open file: %s", filepath);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    // Set MIME type using actual file path
    httpd_resp_set_type(req, get_mime_type(filepath));

    // Send file in chunks
    char buffer[1024];
    size_t read_bytes;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        if (httpd_resp_send_chunk(req, buffer, read_bytes) != ESP_OK)
        {
            fclose(file);
            ESP_LOGE("HTTP", "Failed to send file chunk");
            return ESP_FAIL;
        }
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(file);

    ESP_LOGI("HTTP", "Served file: %s", filepath);
    return ESP_OK;
}

// ===== HTTP Server Task =====

static void http_server_task(void *arg)
{
    ESP_LOGI("HTTP", "Starting HTTP server task");
    static httpd_handle_t server = NULL;
    if (server)
    {
        httpd_stop(server);
        server = NULL;
    }
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&server, &server_config));

    // Store server handle for SSE
    context.http_server = server;

    // /set_cmd POST handler
    httpd_uri_t set_cmd_uri = {
        .uri = "/set_cmd",
        .method = HTTP_POST,
        .handler = set_cmd_handler,
        .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set_cmd_uri));

    // /status GET handler for polling
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));

    // Universal file handler for all requests
    httpd_uri_t file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = file_handler,
        .user_ctx = NULL};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &file_uri));

    ESP_LOGI("HTTP", "HTTP server started with SPIFFS file serving");
    vTaskDelete(NULL);
}

// ===== I2C Slave Task =====

typedef enum
{
    I2C_SLAVE_EVT_RX,
    I2C_SLAVE_EVT_TX
} i2c_slave_event_t;

// I2C slave request callback
static bool i2c_slave_request_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_request_event_data_t *evt_data, void *arg)
{
    i2c_slave_event_t evt = I2C_SLAVE_EVT_TX;
    BaseType_t xTaskWoken = 0;
    // You can prepare data to send back to master here
    xQueueSendFromISR(context.event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}

// I2C slave receive callback
static bool i2c_slave_receive_cb(i2c_slave_dev_handle_t i2c_slave, const i2c_slave_rx_done_event_data_t *evt_data, void *arg)
{
    i2c_slave_event_t evt = I2C_SLAVE_EVT_RX;
    BaseType_t xTaskWoken = 0;
    // Allocate buffer for command and copy data
    uint8_t *cmd_buf = (uint8_t *)malloc(evt_data->length + 1); // +1 for length
    if (cmd_buf)
    {
        cmd_buf[0] = evt_data->length;
        memcpy(&cmd_buf[1], evt_data->buffer, evt_data->length);
        if (xQueueSendFromISR(context.cmd_queue, &cmd_buf, &xTaskWoken) != pdPASS)
        {
            // cmd_queue full, dropping command
            free(cmd_buf);
            esp_rom_printf("cmd_queue full, dropping command\n");
        }
    }
    xQueueSendFromISR(context.event_queue, &evt, &xTaskWoken);
    return xTaskWoken;
}

static void i2c_slave_task(void *arg)
{
    i2c_slave_dev_handle_t slave_handle = (i2c_slave_dev_handle_t)context.slave_handle;

    while (true)
    {
        i2c_slave_event_t evt;
        if (xQueueReceive(context.event_queue, &evt, 10) == pdPASS)
        {
            if (evt == I2C_SLAVE_EVT_TX)
            {
                uint32_t write_len = 0;
                ESP_ERROR_CHECK(i2c_slave_write(slave_handle, &context.ret_cmd, 1, &write_len, -1));
                ESP_LOGI("I2C", "Received TX event, sending queued ret_cmd: 0x%02X", context.ret_cmd);
            }
            if (evt == I2C_SLAVE_EVT_RX)
            {
                uint8_t *cmd_buf = NULL;
                while (xQueueReceive(context.cmd_queue, &cmd_buf, 0) == pdPASS)
                {
                    uint8_t cmd_len = cmd_buf[0];
                    uint8_t *cmd_data = &cmd_buf[1];
                    uint8_t cmd = cmd_data[0];
                    ESP_LOGI("I2C", "Processing RX cmd 0x%02X of length %d", cmd, cmd_len);
                    switch (cmd)
                    {
                    case CMD_WIFI_UID:
                    {
                        if (context.wifi_started)
                        {
                            ESP_LOGW("I2C", "Cannot set web UID while WiFi is started");
                            break;
                        }
                        if (cmd_len > 2)
                        {
                            uint16_t uid = (cmd_data[1] << 8) | cmd_data[2];
                            context.web_uid = uid;
                            ESP_LOGI("I2C", "Web uid set to %d", context.web_uid);
                        }
                        break;
                    }
                    case CMD_WIFI_SSID:
                    {
                        if (context.wifi_started)
                        {
                            ESP_LOGW("I2C", "Cannot set WiFi SSID while WiFi is started");
                            break;
                        }
                        if (cmd_len > 1)
                        {
                            size_t ssid_len = cmd_len - 1;
                            if (ssid_len > WIFI_SSID_MAXLEN)
                                ssid_len = WIFI_SSID_MAXLEN;
                            memcpy(context.wifi_ssid, &cmd_data[1], ssid_len);
                            context.wifi_ssid[ssid_len] = '\0';
                            ESP_LOGI("I2C", "Wifi SSID set to %s", context.wifi_ssid);
                        }
                        break;
                    }
                    case CMD_WIFI_PASS:
                    {
                        if (context.wifi_started)
                        {
                            ESP_LOGW("I2C", "Cannot set WiFi password while WiFi is started");
                            break;
                        }
                        if (cmd_len > 1)
                        {
                            size_t pass_len = cmd_len - 1;
                            if (pass_len > WIFI_PASS_MAXLEN)
                                pass_len = WIFI_PASS_MAXLEN;
                            memcpy(context.wifi_pass, &cmd_data[1], pass_len);
                            context.wifi_pass[pass_len] = '\0';
                            ESP_LOGI("I2C", "Wifi Password set to %s", context.wifi_pass);
                        }
                        break;
                    }
                    case CMD_WIFI_START:
                    {
                        if (context.wifi_started)
                        {
                            ESP_LOGW("I2C", "WiFi already started");
                            break;
                        }
                        wifi_init_sta();
                        context.wifi_started = true;

                        // Restart HTTP server if needed
                        xTaskCreate(http_server_task, "http_server_task", 4096, NULL, 6, NULL);
                        break;
                    }
                    case CMD_SENSOR_READ:
                    {
                        // Trigger sensor read logic
                        // Simulate reading 8 float sensors (replace with real sensor code)
                        xSemaphoreTake(context.ret_cmd_mutex, portMAX_DELAY);
                        for (int i = 0; i < 8; ++i) {
                            // Example: random float between 0 and 100
                            context.sensor_data[i] = (float)(esp_random() % 10000) / 100.0f;
                        }
                        xSemaphoreGive(context.ret_cmd_mutex);
                        break;
                    }
                    default:
                    {
                        // Unknown command
                        break;
                    }
                    }
                    free(cmd_buf);
                }
            }
        }
    }
    vTaskDelete(NULL);
}

static void print_wifi_info_task(void *arg)
{
    while (true)
    {
        printf("SSID: %s\n", context.wifi_ssid);
        printf("Password: %s\n", context.wifi_pass);
        printf("UID: %d\n", context.web_uid);

        // WiFi status
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
        {
            printf("WiFi Status: Connected to %s\n", ap_info.ssid);
        }
        else
        {
            printf("WiFi Status: Not connected\n");
        }

        // IP address
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
        {
            printf("IP Address: %d.%d.%d.%d\n",
                   ip4_addr1_16(&ip_info.ip),
                   ip4_addr2_16(&ip_info.ip),
                   ip4_addr3_16(&ip_info.ip),
                   ip4_addr4_16(&ip_info.ip));
        }
        else
        {
            printf("IP Address: Not assigned\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // Initialize netif and event loop ONCE
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize SPIFFS
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true};

    esp_err_t ret = esp_vfs_spiffs_register(&spiffs_conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE("SPIFFS", "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
    }
    else
    {
        ESP_LOGI("SPIFFS", "SPIFFS mounted successfully");

        // Check SPIFFS usage
        size_t total = 0, used = 0;
        ret = esp_spiffs_info("storage", &total, &used);
        if (ret == ESP_OK)
        {
            ESP_LOGI("SPIFFS", "Partition size: total: %d, used: %d", total, used);
        }
    }

    // I2C slave config
    // using I2C slave version 2, be sure to check it in component config -> I2C
    i2c_slave_config_t conf = {
        .i2c_port = I2C_SLAVE_NUM,
        .sda_io_num = I2C_SLAVE_SDA_IO,
        .scl_io_num = I2C_SLAVE_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 150,
        .receive_buf_depth = 150,
        .slave_addr = ESP_SLAVE_ADDR,
    };

    context.ret_cmd_mutex = xSemaphoreCreateMutex();
    // Create RX command queue (pointer to malloc'd buffers)
    context.cmd_queue = xQueueCreate(16, sizeof(uint8_t *));
    if (context.cmd_queue == NULL)
    {
        ESP_LOGE("I2C", "Failed to create RX command queue");
        return;
    }

    ESP_ERROR_CHECK(i2c_new_slave_device(&conf, &context.slave_handle));

    // Create event queue for RX/TX events
    context.event_queue = xQueueCreate(16, sizeof(i2c_slave_event_t));
    if (context.event_queue == NULL)
    {
        ESP_LOGE("I2C", "Failed to create event queue");
        return;
    }

    // Register callback in a task
    i2c_slave_event_callbacks_t cbs = {
        .on_request = i2c_slave_request_cb,
        .on_receive = i2c_slave_receive_cb,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(context.slave_handle, &cbs, &context));

    ESP_LOGI("I2C", "I2C slave initialized on SCL %d, SDA %d", I2C_SLAVE_SCL_IO, I2C_SLAVE_SDA_IO);

    i2c_slave_dev_handle_t slave_handle = (i2c_slave_dev_handle_t)context.slave_handle;
    uint32_t write_len = 0;
    context.ret_cmd = 0b00000000; // Initialize ret_cmd to 0
    ESP_ERROR_CHECK(i2c_slave_write(slave_handle, &context.ret_cmd, 1, &write_len, 1000));

    xTaskCreate(i2c_slave_task, "i2c_slave_task", 4 * 1024, &context, 10, NULL);
    // xTaskCreate(print_wifi_info_task, "print_wifi_info_task", 4096, &context, 5, NULL);
}