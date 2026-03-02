#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "esp_zigbee_core.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_identify.h"
#include "zcl/esp_zigbee_zcl_temperature_meas.h"

static const char *TAG = "MMW_ZB";

// ------------------- User config -------------------
// HMMD UART pins (pick safe GPIOs you have available)
#define HMMD_UART_NUM        UART_NUM_1
#define HMMD_UART_TX_GPIO    11
#define HMMD_UART_RX_GPIO    10
#define HMMD_UART_BAUD       115200

// Zigbee endpoint
#define HA_ENDPOINT          1

// Publish cadence
#define PUBLISH_PERIOD_MS    5000

// Manufacturer/model shown in ZHA device info
#define MANUFACTURER_NAME    "ChrisLabs"
#define MODEL_ID             "C6_HMMD_Distance"
#define MAX_CHILDREN         10
#define INSTALLCODE_POLICY_ENABLE false

#define ESP_ZB_ZR_CONFIG()                                      \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,              \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,       \
        .nwk_cfg.zczr_cfg = {                                   \
            .max_children = MAX_CHILDREN,                       \
        },                                                      \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()   \
    {                                   \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()        \
    {                                       \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }
// ---------------------------------------------------

// Latest parsed distance in cm (0..400 for 4m)
static float g_range_cm = -1.0f;
static bool  g_have_range = false;
static bool  g_zb_joined = false;

// ---------- UART (HMMD) ----------
static void hmmd_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = HMMD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(HMMD_UART_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(HMMD_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(HMMD_UART_NUM, HMMD_UART_TX_GPIO, HMMD_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "HMMD UART init OK (TX=%d RX=%d @%d)",
             HMMD_UART_TX_GPIO, HMMD_UART_RX_GPIO, HMMD_UART_BAUD);
}

static void hmmd_read_task(void *arg)
{
    (void)arg;
    uint8_t data[256];

    while (1) {
        int len = uart_read_bytes(HMMD_UART_NUM, data, sizeof(data) - 1, pdMS_TO_TICKS(200));
        if (len > 0) {
            data[len] = '\0';

            // Your earlier parsing style: look for "Range "
            char *range_str = strstr((char *)data, "Range ");
            if (range_str) {
                float range_cm = 0.0f;
                if (sscanf(range_str, "Range %f", &range_cm) == 1) {
                    g_range_cm = range_cm;
                    g_have_range = true;
                    // ESP_LOGI(TAG, "Parsed range_cm=%.1f", g_range_cm);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------- Zigbee device ----------
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p = signal_struct->p_app_signal;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*p;
    esp_err_t status = signal_struct->esp_err_status;

    switch (sig) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee stack ready, starting network steering...");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (status == ESP_OK) {
                g_zb_joined = true;
                ESP_LOGI(TAG, "Joined Zigbee network OK");
            } else {
                g_zb_joined = false;
                ESP_LOGW(TAG, "Join failed (%d), retrying...", status);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            }
            break;

        case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
            if (status == ESP_OK) {
                ESP_LOGI(TAG, "Production config loaded");
            } else {
                ESP_LOGW(TAG, "No production config found (expected if zb_fct is empty)");
            }
            break;

        default:
            ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                     esp_zb_zdo_signal_to_string(sig), sig, esp_err_to_name(status));
            break;
    }
}

static esp_zb_ep_list_t *create_endpoint(void)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();

    // Basic
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(NULL);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_ID));
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Identify (optional but common)
    esp_zb_cluster_list_add_identify_cluster(
        cluster_list,
        esp_zb_identify_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );

    // Temperature Measurement cluster (server) -> ZHA will create a sensor entity.
    esp_zb_cluster_list_add_temperature_meas_cluster(
        cluster_list,
        esp_zb_temperature_meas_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE
    );

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = HA_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_ep_list_add_ep(ep_list, cluster_list, ep_cfg);
    return ep_list;
}

// Publish range as "temperature" MeasuredValue (0.01 units)
// We send cm as the raw value so HA shows meters (because 100 -> 1.00)
static void publish_range(void)
{
    // Zigbee temperature MeasuredValue uses int16; 0x8000 means invalid/unknown.
    int16_t measured;

    if (!g_have_range || g_range_cm < 0.0f) {
        measured = (int16_t)0x8000;
    } else {
        // Clamp to something sane (0..1000cm = 10m)
        float cm = g_range_cm;
        if (cm < 0) cm = 0;
        if (cm > 1000) cm = 1000;

        measured = (int16_t)(cm); // cm -> displayed as cm/100 => meters (but labeled °C)
    }
ESP_LOGI(TAG,"measured %d",measured);
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        HA_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,  // MeasuredValue :contentReference[oaicite:2]{index=2}
        &measured,
        false
    );

    // Force a report now (reliable even if coordinator doesn't configure reporting)
    esp_zb_zcl_report_attr_cmd_t rep = {
        .zcl_basic_cmd = {
            .src_endpoint = HA_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
    };
    esp_zb_zcl_report_attr_cmd_req(&rep);
    esp_zb_lock_release();
}

static void publish_task(void *arg)
{
    (void)arg;
    while (1) {
        if (g_zb_joined) {
            publish_range();
        }
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_PERIOD_MS));
    }
}

static void zigbee_task(void *pvParameters)
{
    (void)pvParameters;

    // Platform init
    esp_zb_platform_config_t platform_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform_cfg));

    // Router config (mains powered -> simplest, reliable)
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZR_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    // Register endpoint
    esp_zb_ep_list_t *ep_list = create_endpoint();
    esp_zb_device_register(ep_list);

    // Start
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    hmmd_uart_init();
    xTaskCreate(hmmd_read_task, "hmmd_read", 4096, NULL, 6, NULL);

    xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 5, NULL);
    xTaskCreate(publish_task, "publish", 2048, NULL, 4, NULL);

    ESP_LOGI(TAG, "Started.");
}
