#include <stdio.h>
#include "adc_sensor.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_task_wdt.h" 
#include "nimBLE.h"

#define ADC_UNIT ADC_UNIT_1
#define ADC_CHANNEL ADC_CHANNEL_4  // GPIO32
#define ADC_ATTEN ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH ADC_BITWIDTH_12
#define ADC_READ_LEN 16
#define TAG "ADC_CONTINUOUS"
#define ADC_THRESHOLD 2300

void continuous_adc_init(adc_continuous_handle_t *handle) {
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 16,
        .conv_frame_size = ADC_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 20 * 1000,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };

    adc_digi_pattern_config_t adc_pattern = {
        .atten = ADC_ATTEN,
        .channel = ADC_CHANNEL,
        .unit = ADC_UNIT,
        .bit_width = ADC_BIT_WIDTH
    };
    
    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = &adc_pattern;

    ESP_ERROR_CHECK(adc_continuous_config(*handle, &dig_cfg));
}

bool IRAM_ATTR adc_conversion_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data) {
    BaseType_t mustYield = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)user_data, &mustYield);
    return (mustYield == pdTRUE);
}

void threshold_task(void *arg) {
    TickType_t last_tick = 0;
    while (1) {
        // Wait indefinitely for the notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Get the current tick count
        TickType_t current_tick = xTaskGetTickCount();
        
        if (last_tick != 0) {
            // Calculate the time difference in milliseconds
            TickType_t time_difference = (current_tick - last_tick) * portTICK_PERIOD_MS;
            ESP_LOGI(TAG, "Time between threshold crossings: %lu ms", time_difference);
            sensor_data = (uint32_t)time_difference;
        }
        
        // Update the last tick count
        last_tick = current_tick;

        // Delay to prevent multiple logs for the same event
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void sensor_func(void){
    adc_continuous_handle_t adc_handle;
    continuous_adc_init(&adc_handle);

    uint8_t result[ADC_READ_LEN] = {0};
    uint32_t ret_num = 0;
    uint8_t flag = 0;

    TaskHandle_t main_task_handle = xTaskGetCurrentTaskHandle();
    TaskHandle_t threshold_task_handle = NULL;

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conversion_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(adc_handle, &cbs, main_task_handle));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

    ESP_LOGI(TAG, "ADC continuous mode started");

    xTaskCreate(threshold_task, "threshold_task", 2048, NULL, 10, &threshold_task_handle);

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            esp_err_t ret = adc_continuous_read(adc_handle, result, ADC_READ_LEN, &ret_num, 0);
            if (ret == ESP_OK) {
                for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                    adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
                    uint32_t chan_num = (p->type1.channel);
                    uint32_t data = p->type1.data;
                    if (chan_num == ADC_CHANNEL) {
                        //ESP_LOGI(TAG, "Channel: %"PRIu32", Value: %"PRIu32, chan_num, data);
                        if (data > ADC_THRESHOLD && flag == 0) {
                            // Notify the threshold task if value exceeds threshold
                            xTaskNotifyGive(threshold_task_handle);
                            flag = 1; 
                        }
                        if (data < (ADC_THRESHOLD-200) && flag == 1){
                            flag = 0; 
                        }
                    } else {
                        ESP_LOGW(TAG, "Invalid data [Channel: %"PRIu32", Value: %"PRIu32"]", chan_num, data);
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(100)); // Delay to avoid flooding the log
            } else if (ret == ESP_ERR_TIMEOUT) {
                break; // Exit the inner loop if no data is available
            }
        }

        // Feed the watchdog
        esp_task_wdt_reset();
    }

    ESP_ERROR_CHECK(adc_continuous_stop(adc_handle));
    ESP_ERROR_CHECK(adc_continuous_deinit(adc_handle));
}
