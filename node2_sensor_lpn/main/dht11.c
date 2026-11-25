#include "dht11.h"

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TIMEOUT_US 200

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/* Tunggu sampai pin mencapai level. Hasil: lama menunggu (us), atau -1 jika timeout. */
static int64_t wait_level(gpio_num_t pin, int level)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > TIMEOUT_US) {
            return -1;
        }
    }
    return esp_timer_get_time() - start;
}

bool dht11_read(gpio_num_t pin, sensor_reading_t *out)
{
    uint8_t data[5] = {0};

    /* Start: tarik LOW minimal 18 ms dengan vTaskDelay agar tidak memblokir CPU/BLE. */
    gpio_set_direction(pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(pin, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    taskENTER_CRITICAL(&mux);
    /* Respons: LOW 80 us, HIGH 80 us, lalu LOW 50 us sebelum bit pertama. */
    bool ok = wait_level(pin, 0) >= 0 && wait_level(pin, 1) >= 0 && wait_level(pin, 0) >= 0;
    for (int i = 0; ok && i < 40; i++) {
        ok = wait_level(pin, 1) >= 0;
        if (!ok) {
            break;
        }
        /* Bit 0 punya HIGH sekitar 27 us, bit 1 sekitar 70 us. */
        int64_t high_us = wait_level(pin, 0);
        if (high_us < 0) {
            ok = false;
            break;
        }
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }
    taskEXIT_CRITICAL(&mux);

    uint8_t sum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (!ok || data[4] != sum || data[0] > 100) {
        return false;
    }
    out->hum_pct = data[0];
    out->temp_c = (int8_t)data[2];
    return true;
}
