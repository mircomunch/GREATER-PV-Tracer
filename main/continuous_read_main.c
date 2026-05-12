#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "soc/soc_caps.h"

/* ------------ USER CONFIGURATION ------------ */
#define DEBOUNCE_MS                         10          // ms
#define ACQ_DURATION_MS                     100         // ms
#define CALIBRATION_SAMPLE_COUNT            256         // I_LOAD samples used to estimate Allegro zero-current offset
#define CALIBRATION_TIMEOUT_MS              200         // ms
#define R1                                  27000.0f    // ohm
#define R2                                  2000.0f     // ohm
#define ALLEGRO_SENS                        0.1f        // V/A
#define ALLEGRO_OFFSET                      0.65f       // V, fallback if sample calibration fails

/* ------------ ADC CONFIGURATION ------------ */
/*
 * ESP32-C6 supports up to 83333 ADC continuous conversions/s in IDF 5.5.
 * With two channels in the pattern, 80 kHz is the total conversion rate,
 * so each channel receives about 40 kSamples/s.
 */
#define ADC_SAMPLE_FREQ_HZ                  (80 * 1000)
#define ADC_CHANNEL_COUNT                   2
#define ACQ_TOTAL_SAMPLE_COUNT              ((ADC_SAMPLE_FREQ_HZ * ACQ_DURATION_MS) / 1000)
#define SAMPLE_BUFF_LEN                     (ACQ_TOTAL_SAMPLE_COUNT / ADC_CHANNEL_COUNT)
#define ADC_READ_BUF_BYTES                  512
#define ADC_STORE_BUF_BYTES                 (ADC_READ_BUF_BYTES * 8)

#define ADC_UNIT                            ADC_UNIT_1
#define ADC_CONV_MODE                       ADC_CONV_SINGLE_UNIT_1
#define ADC_ATTEN                           ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH                       SOC_ADC_DIGI_MAX_BITWIDTH
#define ADC_FALLBACK_VREF_MV                3300

#if (ADC_SAMPLE_FREQ_HZ > SOC_ADC_SAMPLE_FREQ_THRES_HIGH)
#error "ADC_SAMPLE_FREQ_HZ is higher than this ESP32-C6/ESP-IDF target supports"
#endif

#if ((ADC_READ_BUF_BYTES % SOC_ADC_DIGI_DATA_BYTES_PER_CONV) != 0)
#error "ADC_READ_BUF_BYTES must be aligned to SOC_ADC_DIGI_DATA_BYTES_PER_CONV"
#endif

/* ------------ GPIO CONFIGURATION ------------ */
// GPIO0 -> VDAC / acquisition enable
#define PIN_GPIO0                           0
#define GPIO_OUTPUT_PIN_SEL                 (1ULL << PIN_GPIO0)
// GPIO4 -> BUTTON, active low
#define BUTTON_PIN                          4
#define GPIO_INPUT_PIN_SEL                  (1ULL << BUTTON_PIN)
#define ESP_INTR_FLAG_DEFAULT               0

#define NOTIFY_BUTTON                       BIT0
#define NOTIFY_TIMER                        BIT1

static const char *TAG = "LOG";

typedef enum {
    SYS_IDLE = 0,
    SYS_ACQUIRE,
    SYS_PROCESS,
    SYS_TRANSMIT,
} system_state_t;

// CHANNEL 1 -> GPIO1 -> V_panel
// CHANNEL 2 -> GPIO2 -> I_LOAD
static adc_channel_t s_channels[ADC_CHANNEL_COUNT] = {
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
};

static uint16_t s_vpanel_raw[SAMPLE_BUFF_LEN];
static uint16_t s_iload_raw[SAMPLE_BUFF_LEN];
static float s_vpanel[SAMPLE_BUFF_LEN];
static float s_iload[SAMPLE_BUFF_LEN];

static size_t s_vpanel_count;
static size_t s_iload_count;
static size_t s_vpanel_dropped;
static size_t s_iload_dropped;

static TaskHandle_t s_acq_task_handle;
static QueueHandle_t s_gpio_evt_queue;
static adc_continuous_handle_t s_adc_handle;
static gptimer_handle_t s_gptimer;
static adc_cali_handle_t s_cali_ch1;
static adc_cali_handle_t s_cali_ch2;

static volatile bool s_acq_timer_expired;
static volatile system_state_t s_sys_state = SYS_IDLE;
static float s_allegro_offset_v = ALLEGRO_OFFSET;

static int raw_to_voltage_mv(adc_cali_handle_t cali_handle, uint16_t raw)
{
    int voltage_mv = 0;

    if (cali_handle != NULL && adc_cali_raw_to_voltage(cali_handle, raw, &voltage_mv) == ESP_OK) {
        return voltage_mv;
    }

    return (int)(((uint32_t)raw * ADC_FALLBACK_VREF_MV) / ((1U << ADC_BIT_WIDTH) - 1U));
}

static void reset_sample_storage(void)
{
    s_vpanel_count = 0;
    s_iload_count = 0;
    s_vpanel_dropped = 0;
    s_iload_dropped = 0;
    memset(s_vpanel_raw, 0, sizeof(s_vpanel_raw));
    memset(s_iload_raw, 0, sizeof(s_iload_raw));
}

static void store_acquisition_sample(const adc_continuous_data_t *sample)
{
    if (!sample->valid || sample->unit != ADC_UNIT) {
        return;
    }

    if (sample->channel == ADC_CHANNEL_1) {
        if (s_vpanel_count < SAMPLE_BUFF_LEN) {
            s_vpanel_raw[s_vpanel_count++] = (uint16_t)sample->raw_data;
        } else {
            s_vpanel_dropped++;
        }
    } else if (sample->channel == ADC_CHANNEL_2) {
        if (s_iload_count < SAMPLE_BUFF_LEN) {
            s_iload_raw[s_iload_count++] = (uint16_t)sample->raw_data;
        } else {
            s_iload_dropped++;
        }
    }
}

static esp_err_t read_and_parse_adc_frame(uint8_t *raw_buf,
                                          adc_continuous_data_t *parsed,
                                          uint32_t timeout_ms,
                                          uint32_t *parsed_count)
{
    uint32_t bytes_read = 0;
    esp_err_t ret = adc_continuous_read(s_adc_handle, raw_buf, ADC_READ_BUF_BYTES, &bytes_read, timeout_ms);
    if (ret != ESP_OK) {
        *parsed_count = 0;
        return ret;
    }

    ret = adc_continuous_parse_data(s_adc_handle, raw_buf, bytes_read, parsed, parsed_count);
    if (ret != ESP_OK) {
        *parsed_count = 0;
    }

    return ret;
}

static esp_err_t calibrate_allegro_offset(void)
{
    uint8_t raw_buf[ADC_READ_BUF_BYTES];
    adc_continuous_data_t parsed[ADC_READ_BUF_BYTES / SOC_ADC_DIGI_RESULT_BYTES];
    uint32_t samples = 0;
    uint64_t sum_mv = 0;
    esp_err_t ret = ESP_OK;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CALIBRATION_TIMEOUT_MS);

    s_allegro_offset_v = ALLEGRO_OFFSET;

    ret = adc_continuous_flush_pool(s_adc_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = adc_continuous_start(s_adc_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    while (samples < CALIBRATION_SAMPLE_COUNT &&
           (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        uint32_t parsed_count = 0;
        ret = read_and_parse_adc_frame(raw_buf, parsed, 20, &parsed_count);
        if (ret == ESP_ERR_TIMEOUT) {
            continue;
        }
        if (ret != ESP_OK) {
            break;
        }

        for (uint32_t i = 0; i < parsed_count && samples < CALIBRATION_SAMPLE_COUNT; i++) {
            if (parsed[i].valid && parsed[i].unit == ADC_UNIT && parsed[i].channel == ADC_CHANNEL_2) {
                sum_mv += raw_to_voltage_mv(s_cali_ch2, (uint16_t)parsed[i].raw_data);
                samples++;
            }
        }
    }

    esp_err_t stop_ret = adc_continuous_stop(s_adc_handle);
    if (ret == ESP_OK && stop_ret != ESP_OK) {
        ret = stop_ret;
    }

    esp_err_t flush_ret = adc_continuous_flush_pool(s_adc_handle);
    if (ret == ESP_OK && flush_ret != ESP_OK) {
        ret = flush_ret;
    }

    if (ret != ESP_OK) {
        return ret;
    }

    if (samples == 0) {
        return ESP_ERR_TIMEOUT;
    }

    s_allegro_offset_v = ((float)sum_mv / (float)samples) / 1000.0f;
    ESP_LOGI(TAG, "I_LOAD offset calibrated with %" PRIu32 " samples: %.4f V",
             samples, s_allegro_offset_v);

    return (samples == CALIBRATION_SAMPLE_COUNT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static size_t process_samples(void)
{
    size_t pairs = (s_vpanel_count < s_iload_count) ? s_vpanel_count : s_iload_count;

    for (size_t i = 0; i < pairs; i++) {
        int vpanel_mv = raw_to_voltage_mv(s_cali_ch1, s_vpanel_raw[i]);
        int iload_mv = raw_to_voltage_mv(s_cali_ch2, s_iload_raw[i]);

        s_vpanel[i] = (vpanel_mv / 1000.0f) * (R1 + R2) / R2;
        s_iload[i] = ((iload_mv / 1000.0f) - s_allegro_offset_v) / ALLEGRO_SENS;
    }

    return pairs;
}

static void transmit_samples(size_t pairs)
{
    printf("V,I\n");
    for (size_t i = 0; i < pairs; i++) {
        printf("%.3f,%.3f\n", s_vpanel[i], s_iload[i]);
    }
    printf("STOP\n");
}

static void acquisition_task(void *arg)
{
    uint8_t raw_buf[ADC_READ_BUF_BYTES];
    adc_continuous_data_t parsed[ADC_READ_BUF_BYTES / SOC_ADC_DIGI_RESULT_BYTES];

    while (1) {
        uint32_t notify_value = 0;
        xTaskNotifyWait(0, UINT32_MAX, &notify_value, portMAX_DELAY);

        if ((notify_value & NOTIFY_BUTTON) == 0 || s_sys_state != SYS_IDLE) {
            continue;
        }

        s_sys_state = SYS_ACQUIRE;
        reset_sample_storage();
        printf("START\n");

        esp_err_t ret = calibrate_allegro_offset();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Sample calibration incomplete (%s), using offset %.4f V",
                     esp_err_to_name(ret), s_allegro_offset_v);
        }

        ret = adc_continuous_flush_pool(s_adc_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC flush failed: %s", esp_err_to_name(ret));
            s_sys_state = SYS_IDLE;
            continue;
        }

        s_acq_timer_expired = false;
        xTaskNotifyWait(0, UINT32_MAX, &notify_value, 0);

        ESP_ERROR_CHECK(gptimer_set_raw_count(s_gptimer, 0));
        gpio_set_level(PIN_GPIO0, 1);
        ESP_ERROR_CHECK(adc_continuous_start(s_adc_handle));
        ESP_ERROR_CHECK(gptimer_start(s_gptimer));

        while (!s_acq_timer_expired) {
            uint32_t parsed_count = 0;
            ret = read_and_parse_adc_frame(raw_buf, parsed, 20, &parsed_count);
            if (ret == ESP_ERR_TIMEOUT) {
                continue;
            }
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ADC read/parse failed: %s", esp_err_to_name(ret));
                break;
            }

            for (uint32_t i = 0; i < parsed_count; i++) {
                store_acquisition_sample(&parsed[i]);
            }
        }

        ESP_ERROR_CHECK(gptimer_stop(s_gptimer));
        ESP_ERROR_CHECK(adc_continuous_stop(s_adc_handle));
        gpio_set_level(PIN_GPIO0, 0);

        s_sys_state = SYS_PROCESS;
        size_t pairs = process_samples();

        if (pairs == 0) {
            ESP_LOGE(TAG, "ADC ERROR: no complete V/I sample pairs");
        } else if (pairs < SAMPLE_BUFF_LEN) {
            ESP_LOGW(TAG, "Short capture: %u V samples, %u I samples, expected %u each",
                     (unsigned)s_vpanel_count, (unsigned)s_iload_count, (unsigned)SAMPLE_BUFF_LEN);
        }

        if (s_vpanel_dropped || s_iload_dropped) {
            ESP_LOGW(TAG, "Dropped samples after buffers filled: V=%u, I=%u",
                     (unsigned)s_vpanel_dropped, (unsigned)s_iload_dropped);
        }

        s_sys_state = SYS_TRANSMIT;
        transmit_samples(pairs);

        xTaskNotifyWait(0, UINT32_MAX, &notify_value, 0);
        s_sys_state = SYS_IDLE;
    }
}

/* ------------ BUTTON INTERRUPT ------------ */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio = (uint32_t)arg;
    BaseType_t high_task_wakeup = pdFALSE;

    xQueueSendFromISR(s_gpio_evt_queue, &gpio, &high_task_wakeup);
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void button_task(void *arg)
{
    uint32_t gpio;
    TickType_t last = 0;

    while (1) {
        if (xQueueReceive(s_gpio_evt_queue, &gpio, portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();

            if (now - last < pdMS_TO_TICKS(DEBOUNCE_MS)) {
                continue;
            }

            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

            if (gpio_get_level(gpio) == 0 && s_sys_state == SYS_IDLE) {
                last = now;
                xTaskNotify(s_acq_task_handle, NOTIFY_BUTTON, eSetBits);
            }
        }
    }
}

static void gpio_init(void)
{
    gpio_config_t out_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = GPIO_OUTPUT_PIN_SEL,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));
    gpio_set_level(PIN_GPIO0, 0);

    gpio_config_t in_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = GPIO_INPUT_PIN_SEL,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_conf));

    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_PIN, gpio_isr_handler, (void *)BUTTON_PIN));
}

/* ------------ ADC ------------ */
static void continuous_adc_init(adc_channel_t *channels,
                                uint8_t channel_num,
                                adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = ADC_STORE_BUF_BYTES,
        .conv_frame_size = ADC_READ_BUF_BYTES,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = ADC_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_MODE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        adc_pattern[i].atten = ADC_ATTEN;
        adc_pattern[i].channel = channels[i];
        adc_pattern[i].unit = ADC_UNIT;
        adc_pattern[i].bit_width = ADC_BIT_WIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%" PRIx8, i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%" PRIx8, i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%" PRIx8, i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

    *out_handle = handle;
}

static bool adc_calibration_init(adc_unit_t unit,
                                 adc_channel_t channel,
                                 adc_atten_t atten,
                                 adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

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

    *out_handle = handle;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success, channel: %d", channel);
    } else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
        ESP_LOGW(TAG, "eFuse not burnt, using raw-to-mV fallback for channel: %d", channel);
    } else {
        ESP_LOGE(TAG, "Calibration init failed for channel %d: %s", channel, esp_err_to_name(ret));
    }

    return calibrated;
}

/* ------------ TIMER ------------ */
static bool IRAM_ATTR timer_alarm_cb(gptimer_handle_t timer,
                                     const gptimer_alarm_event_data_t *edata,
                                     void *ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;

    s_acq_timer_expired = true;
    xTaskNotifyFromISR(s_acq_task_handle, NOTIFY_TIMER, eSetBits, &high_task_wakeup);

    return high_task_wakeup == pdTRUE;
}

static void timer_init(void)
{
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &s_gptimer));

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = ACQ_DURATION_MS * 1000,
        .flags.auto_reload_on_alarm = false,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_gptimer, &alarm_config));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_alarm_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_gptimer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_gptimer));
}

void app_main(void)
{
    s_gpio_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    if (s_gpio_evt_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create GPIO event queue");
        return;
    }

    timer_init();
    continuous_adc_init(s_channels, ADC_CHANNEL_COUNT, &s_adc_handle);

    adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_1, ADC_ATTEN, &s_cali_ch1);
    adc_calibration_init(ADC_UNIT_1, ADC_CHANNEL_2, ADC_ATTEN, &s_cali_ch2);

    xTaskCreate(acquisition_task, "acq_task", 6144, NULL, 9, &s_acq_task_handle);
    xTaskCreate(button_task, "btn_task", 2048, NULL, 10, NULL);

    gpio_init();
}
