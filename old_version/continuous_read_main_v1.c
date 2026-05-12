/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"

/* ------------ USER CONFIGURATION ------------*/
#define DEBOUNCE_MS                         10          // ms
#define ton                                 100         // ms
#define R1                                  27000.0     // ohm
#define R2                                  2000.0      // ohm
#define ALLEGRO_SENS                        0.1         // V/A
#define ALLEGRO_OFFSET                      0.65        // V
/* ------------ ADC CONFIGURATION ------------*/
#define ADC_SAMPLE_FREQ                     5 * 1000    //Sample frequency in Hz
#define SAMPLE_BUFF_LEN                     500         //Sample buffer length in samples
#define SAMPLE_BUFF_LEN_BYTE                (SAMPLE_BUFF_LEN * SOC_ADC_DIGI_RESULT_BYTES) //Sample buffer length in bytes
#define SAMPLE_FRAME_NUM                    4           //Number of sample frames

#define ADC_UNIT                            ADC_UNIT_1
#define ADC_CONV_MODE                       ADC_CONV_SINGLE_UNIT_1
#define ADC_ATTEN                           ADC_ATTEN_DB_0
#define ADC_BIT_WIDTH                       SOC_ADC_DIGI_MAX_BITWIDTH
/* ------------ GPIO CONFIGURATION ------------*/
// GPIO0 -> VDAC
#define PIN_GPIO0 0
#define GPIO_OUTPUT_PIN_SEL  (1ULL<<PIN_GPIO0)
// GPIO4 -> BUTTON
#define BUTTON_PIN 4
#define GPIO_INPUT_PIN_SEL  (1ULL<<BUTTON_PIN)

// CHANNEL 1 -> GPIO1 -> V_panel
// CHANNEL 2 -> GPIO2 -> I_LOAD
static adc_channel_t channel[2] = {ADC_CHANNEL_1, ADC_CHANNEL_2};
adc_cali_handle_t adc1_cali_chan1_handle = NULL;
adc_cali_handle_t adc1_cali_chan2_handle = NULL;

#define ESP_INTR_FLAG_DEFAULT 0
static QueueHandle_t gpio_evt_queue = NULL;
static TaskHandle_t button_task_handle = NULL;

static TaskHandle_t adc_cb_handle;
adc_continuous_handle_t adc_handle = NULL;

gptimer_handle_t gptimer = NULL;

static const char *TAG = "LOG";

static void acquisition_start_task(void* arg)
{
    for (;;) {
        /* Block here until notified */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // SERIAL OUTPUT 'START'
        printf("START\n");

        // SET GPIO0 HIGH
        gpio_set_level(PIN_GPIO0, 1);

        // START ADC CONTINUOUS CONVERSION
        ESP_ERROR_CHECK(adc_continuous_start(adc_handle));

        // START TIMER for ton
        ESP_ERROR_CHECK(gptimer_start(gptimer));
    }
}

static void acquisition_stop_task(void* arg)
{
    for (;;) {
        /* Block here until notified */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // STOP ADC CONTINUOUS CONVERSION
        ESP_ERROR_CHECK(adc_continuous_stop(adc_handle));

        // TODO: SEND ADC results

        // SERIAL OUTPUT 'STOP'
        printf("STOP\n");

        // SET GPIO0 LOW
        gpio_set_level(PIN_GPIO0, 0);
    }
}

/* ------------ BUTTON INTERRUPT ------------*/
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_button(void* arg)
{
    uint32_t io_num;
    TickType_t last_press_tick = 0;

    for (;;) {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {

            TickType_t now = xTaskGetTickCount();

            /* Debounce: ignore rapid retriggers */
            if ((now - last_press_tick) < pdMS_TO_TICKS(DEBOUNCE_MS)) {
                continue;
            }

            /* Small delay to allow signal to settle */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            /* Confirm button is still active (active-low) */
            if (gpio_get_level(io_num) == 0) {
                last_press_tick = now;

                printf("Button press confirmed on GPIO %" PRIu32 "\n", io_num);

                /* Notify acquisition task */
                xTaskNotifyGive(button_task_handle);
            }
        }
    }
}

static void gpio_init(){
    // GPIO configuration
    gpio_config_t io_conf = {};

    // Output GPIO
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    gpio_config(&io_conf);
    
    // Input GPIO with interrupt
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Queue for ISR -> GPIO task
    gpio_evt_queue = xQueueCreate(1, sizeof(uint32_t));

    //Create acquisition task FIRST (needed for handle)
    xTaskCreate(acquisition_start_task, "acq_start_task", 2048, NULL, 5, &button_task_handle);

    /* Create GPIO event task */
    xTaskCreate(gpio_task_button, "gpio_task", 2048, NULL, 10, NULL);

    /* Install ISR */
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BUTTON_PIN, gpio_isr_handler, (void*) BUTTON_PIN);
}

/* ------------ ADC ------------*/
// Callback function to notify that ADC frame is done
static bool IRAM_ATTR adc_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notify to 'adc_cb_handle' that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(adc_cb_handle, &mustYield);

    return (mustYield == pdTRUE);
}

static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;

    // Handle configuration: Buffer and Frame size
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = SAMPLE_BUFF_LEN_BYTE,
        .conv_frame_size = SAMPLE_FRAME_NUM,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    // Frequency and pattern configuration
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = ADC_SAMPLE_FREQ,
        .conv_mode = ADC_CONV_MODE,
    };

    // Attenuation and channel configuration
    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        adc_pattern[i].atten = ADC_ATTEN;
        adc_pattern[i].channel = channel[i] & 0x7;
        adc_pattern[i].unit = ADC_UNIT;
        adc_pattern[i].bit_width = ADC_BIT_WIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%"PRIx8, i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%"PRIx8, i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%"PRIx8, i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

    *out_handle = handle;

    // Callback registration
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = adc_conv_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
}

// TODO: set proper arguments
// void convert_adc_raw_data(uint32_t raw_data_ch1, uint32_t raw_data_ch2, float *V_panel, float *I_load)
// {
//     // Convert raw ADC data to voltage and current values
//     float v_adc_ch1 = (raw_data_ch1 / 4095.0) * 3.3; // Assuming 12-bit ADC and 3.3V reference
//     float v_adc_ch2 = (raw_data_ch2 / 4095.0) * 3.3; // Assuming 12-bit ADC and 3.3V reference

//     // Voltage at the panel (considering voltage divider)
//     *V_panel = v_adc_ch1 * (R1 + R2) / R2;
//     // Current through the load (considering shunt resistor)
//     *I_load = v_adc_ch2 / ALLEGRO_SENS;
// }

// ADC Calibration
static bool adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

    if (!calibrated) {
        ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK) {
            calibrated = true;
        }
    }

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success");
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    } else {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

static void adc_calibration_deinit(adc_cali_handle_t handle)
{
    ESP_LOGI(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));
}

/* ------------ TIMER ------------ */
static bool IRAM_ATTR timer_on_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    // STOP TIMER
    gptimer_stop(timer);
    vTaskNotifyGiveFromISR(acquisition_stop_task, NULL);

    return false;
}

static void timer_init()
{
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT, // Select the default clock source
        .direction = GPTIMER_COUNT_UP,      // Counting direction is up
        .resolution_hz = 1 * 1000 * 1000,   // Resolution is 1 MHz, i.e., 1 tick equals 1 microsecond
    };
    // Create a timer instance
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));
    
    gptimer_alarm_config_t alarm_config = {
        .alarm_count = ton * 1000, // in ms
        .flags.auto_reload_on_alarm = false, // Disable auto-reload function
    };
    // Set the timer's alarm action
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_on_alarm_cb, // Call the user callback function when the alarm event occurs
    };
    // Register timer event callback functions, allowing user context to be carried
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, NULL));
    // Enable the timer
    ESP_ERROR_CHECK(gptimer_enable(gptimer));
}

void app_main(void)
{
    gpio_init();
    timer_init();
    adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_1, ADC_ATTEN, &adc1_cali_chan1_handle);
    adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_2, ADC_ATTEN, &adc1_cali_chan2_handle);


    // ADC data processing task
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[SAMPLE_BUFF_LEN] = {0};
    memset(result, 0xcc, SAMPLE_BUFF_LEN);

    adc_cb_handle = xTaskGetCurrentTaskHandle();

    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &adc_handle);

    while (1) {

        /**
         * This is to show you the way to use the ADC continuous mode driver event callback.
         * This `ulTaskNotifyTake` will block when the data processing in the task is fast.
         * However in this example, the data processing (print) is slow, so you barely block here.
         *
         * Without using this event callback (to notify this task), you can still just call
         * `adc_continuous_read()` here in a loop, with/without a certain block timeout.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (1) {
            ret = adc_continuous_read(adc_handle, result, SAMPLE_BUFF_LEN_BYTE, &ret_num, 0);
            if (ret == ESP_OK) {
                ESP_LOGI("TASK", "ret is %x, ret_num is %"PRIu32" bytes", ret, ret_num);

                adc_continuous_data_t parsed_data[ret_num / SOC_ADC_DIGI_RESULT_BYTES];
                uint32_t num_parsed_samples = 0;

                esp_err_t parse_ret = adc_continuous_parse_data(adc_handle, result, ret_num, parsed_data, &num_parsed_samples);
                if (parse_ret == ESP_OK) {
                    for (int i = 0; i < num_parsed_samples; i++) {
                        if (parsed_data[i].valid) {
                            if(parsed_data[i].channel == ADC_CHANNEL_1) {
                                // Vpanel
                                int voltage_Vpanel = 0;
                                ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan1_handle, parsed_data[i].raw_data, &voltage_Vpanel));
                                float Vpanel = (voltage_Vpanel / 1000.0) * (R1 + R2) / R2; // Convert to Vpanel
                                printf("V=%.3f\n", Vpanel);
                            } else if (parsed_data[i].channel == ADC_CHANNEL_2) {
                                // I_LOAD
                                int voltage_Iload = 0;
                                ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc1_cali_chan2_handle, parsed_data[i].raw_data, &voltage_Iload));
                                float Iload = ((voltage_Iload / 1000.0) - ALLEGRO_OFFSET) / ALLEGRO_SENS; // Convert millivolts to volts
                                printf("I=%.3f\n", Iload);
                            }
                        } else {
                            ESP_LOGW(TAG, "Invalid data [ADC%d_Ch%d_%"PRIu32"]",
                                     parsed_data[i].unit + 1,
                                     parsed_data[i].channel,
                                     parsed_data[i].raw_data);
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Data parsing failed: %s", esp_err_to_name(parse_ret));
                }

                /**
                 * Because printing is slow, so every time you call `ulTaskNotifyTake`, it will immediately return.
                 * To avoid a task watchdog timeout, add a delay here. When you replace the way you process the data,
                 * usually you don't need this delay (as this task will block for a while).
                 */
                vTaskDelay(100);
            } else if (ret == ESP_ERR_TIMEOUT) {
                //We try to read `SAMPLE_BUFF_LEN` until API returns timeout, which means there's no available data
                break;
            }
        }
    }

    // STOP ADC CONTINUOUS CONVERSION
    ESP_ERROR_CHECK(adc_continuous_stop(adc_handle));
    ESP_ERROR_CHECK(adc_continuous_deinit(adc_handle));
}
