/* Edge Impulse ingestion SDK
 * Copyright (c) 2022 EdgeImpulse Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Include ----------------------------------------------------------------- */
#include "driver/gpio.h"
#include "sdkconfig.h"

#include <stdio.h>
#include "includes.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <tcpip_adapter.h>

#include "lwip/err.h"
#include "lwip/sys.h"

#include "ei_device_espressif_esp32.h"

#include "ei_at_handlers.h"
#include "ei_classifier_porting.h"
#include "ei_run_impulse.h"

#include "ei_analogsensor.h"
#include "ei_inertial_sensor.h"

#include "esp_http_server.h"
#include "img_converters.h"
#include "mdns.h"

#include "mqtt_client.h"


/************************************* WIFI ***********************************/

#define EXAMPLE_ESP_WIFI_SSID      "TP-Link_6B03"
#define EXAMPLE_ESP_WIFI_PASS      "ra1nboots"
//#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY
#define HOSTNAME                    "chickencam"


#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";

static int s_retry_num = 0;

/************************************** MQTT ***********************************/

#define MQTT_HOST "homeassistant"
#define MQTT_USERNAME "mqtt"
#define MQTT_PASSWORD "mqtt"
#define PUB_TOPIC "chicken_count"


#define PART_BOUNDARY "123456789000000000000987654321"


/******************************** Variables **********************************/
#define ALPHA 0.1


static QueueHandle_t xQueueHttpFrame = NULL;
SemaphoreHandle_t xBinarySemaphore;
SemaphoreHandle_t xSemaphore;
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

struct buf_cont
{
    uint8_t *buf = NULL;
    uint32_t *size = 0;
};

struct buf_cont picture;

/********************************** Prototypes ***********************************/

void startCameraServer();
static esp_err_t stream_handler(httpd_req_t *req);
void wifi_init_sta(void);
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);
static void initialise_mdns(void);
struct esp_ip4_addr query_mdns_host(const char *host_name);


/**********************************  Main      ***********************************/



extern "C" int app_main()
{
    int detected_count = 0;
    float filtered_count = 0;
    float last_filtered_count = 0;

    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    ei_printf("Connecting to wifi\n");
    wifi_init_sta();

    initialise_mdns();


    // Resolve MQTT hostname
    struct esp_ip4_addr addr;
    addr.addr = 0;
    addr = query_mdns_host(MQTT_HOST);
    char ip[32];
    sprintf(ip, IPSTR, IP2STR(&addr));
    printf(": %s resolved to: %s\n", MQTT_HOST, ip);

    // Setup MQTT
    const esp_mqtt_client_config_t mqtt_cfg = {
        .host = ip,
        .uri = NULL,
        .port = 1883,
        .set_null_client_id = false,                /*!< Selects a NULL client id */
        .client_id = NULL,
        .username = MQTT_USERNAME,
        .password = MQTT_PASSWORD,
        .refresh_connection_after_ms = 500,
        //.transport = MQTT_TRANSPORT_OVER_TCP,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    //esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_err_t err = esp_mqtt_client_start(client);
    printf("MQTT init: %s", esp_err_to_name(err));

    char test[] = "test count";
    int len = strlen(test);
    esp_mqtt_client_publish(client, PUB_TOPIC, test, len, 0, 1);

    /* Create a mutex type semaphore. */
    xSemaphore = xSemaphoreCreateMutex();

    
    ei_printf("\n\n\n");
    
    ei_printf(
        "Hello from Edge Impulse Device SDK.\r\n"
        "Compiled on %s %s\r\n",
        __DATE__,
        __TIME__);

    // Start impulse. Not continous, w/o debug,
    ei_start_impulse(false, false, false);
    // ei_run_impulse();

    startCameraServer();
    

    while (1)
    {
        // filtdata.x = ALFA * acdata.x + (1 - ALFA) * lastdata.x;
        xSemaphoreTake(xSemaphore,portMAX_DELAY);
        detected_count = ei_run_impulse(xSemaphore);

        // Filter/ Average the counted chickens
        filtered_count = ALPHA * detected_count + (1- ALPHA) * last_filtered_count;
        last_filtered_count = filtered_count;

        // Prepare payload
        char payload[32];
        sprintf(payload, "%.2f", filtered_count);
        int len = strlen(payload);

        // Transmit over MQTT
        esp_mqtt_client_publish(client, PUB_TOPIC, payload, len, 0, 1);
        ei_sleep(200);
        
        
    }
}

/******************************     mDNS    *******************************/

static void initialise_mdns(void)
{
        //initialize mDNS
    ESP_ERROR_CHECK( mdns_init() );
    //set mDNS hostname (required if you want to advertise services)
    ESP_ERROR_CHECK( mdns_hostname_set(HOSTNAME) );
    ESP_LOGI(TAG, "mdns hostname set to: [%s]", HOSTNAME);

}
struct esp_ip4_addr query_mdns_host(const char *host_name)
{
    ESP_LOGI(TAG, "Query A: %s.local", host_name);

    struct esp_ip4_addr addr;
    addr.addr = 0;

    esp_err_t err = mdns_query_a(host_name, 2000,  &addr);
    if (err) {
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: Host was not found!", esp_err_to_name(err));
            return addr;
        }
        ESP_LOGE(TAG, "Query Failed: %s", esp_err_to_name(err));
        return addr;
    }

    ESP_LOGI(TAG, "Query A: %s.local resolved to: " IPSTR, host_name, IP2STR(&addr));
    return addr;
}


/******************************  Web Server *******************************/

static esp_err_t stream_handler(httpd_req_t *req)
{
    //struct buf_cont *inp_jpeg = NULL;
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[64];
    

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK)
    {
        return res;
    }

    while (true)
    {
        ei_sleep(1000);
        xSemaphoreTake(xSemaphore,portMAX_DELAY);
        fb = esp_camera_fb_get();

        //xQueueReceive(xQueueHttpFrame, &inp_jpeg, portMAX_DELAY);
        
        if (!fb)
        {
            printf("Camera capture failed\n");
            res = ESP_FAIL;
        }
        else
        {
            //fb_pic.format = GRAYSCALE;
            //bool jpeg_converted = frame2jpg(fb_pic, 80, &_jpg_buf, &_jpg_buf_len);
            //fmt2jpg(inp_jpeg->buf, *inp_jpeg->size, 320, 320, PIXFORMAT_GRAYSCALE, 100, &_jpg_buf, &_jpg_buf_len);
            //ei_free(inp_jpeg->buf);
            //inp_jpeg->buf = NULL;
            //*inp_jpeg->size = 0;
            
            if (fb->width > 400)
            {
                if (fb->format != PIXFORMAT_JPEG)
                {
                    bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                    esp_camera_fb_return(fb);
                    fb = NULL;
                    if (!jpeg_converted)
                    {
                        printf("JPEG compression failed\n");
                        res = ESP_FAIL;
                    }
                }
                else
                {
                    _jpg_buf_len = fb->len;
                    _jpg_buf = fb->buf;
                }
            }
            
           
        }
        if (res == ESP_OK)
        {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK)
        {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        
        
        if (fb)
        {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf)
        {
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        

        xSemaphoreGive(xSemaphore);

        if (res != ESP_OK)
        {
            break;
        }
        // Serial.printf("MJPG: %uB\n",(uint32_t)(_jpg_buf_len));
    }
    return res;
}

void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = NULL};
    
    tcpip_adapter_ip_info_t ipInfo; 
    //char str[32];
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ipInfo);
    //sprintf(str, "%x", ipInfo.ip.addr);
    //ei_printf("Starting web server on port: '%d'\n", config.server_port);
    printf("Connect to http server at: " IPSTR "\n", IP2STR(&ipInfo.ip));
    if (httpd_start(&stream_httpd, &config) == ESP_OK)
    {
        httpd_register_uri_handler(stream_httpd, &index_uri);
    }
}

/******************************************* WIFI ********************************************/


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < 20) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

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

    wifi_config_t wifi_config = { .sta = {
            EXAMPLE_ESP_WIFI_SSID,
            EXAMPLE_ESP_WIFI_PASS,
            //.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
	     * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            
            //.sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

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
    if (bits & WIFI_CONNECTED_BIT) {
        printf(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}
