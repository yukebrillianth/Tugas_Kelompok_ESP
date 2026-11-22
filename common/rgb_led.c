#include "rgb_led.h"

#include "esp_check.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#define TAG "RGB"
/* 10 MHz, satu tick 0.1 us, cukup untuk timing WS2812. */
#define RMT_RESOLUTION_HZ 10000000

static rmt_channel_handle_t led_chan;
static rmt_encoder_handle_t led_encoder;

esp_err_t rgb_led_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_RGB_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_cfg, &led_chan), TAG, "channel RMT gagal");

    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &led_encoder), TAG, "encoder gagal");
    ESP_RETURN_ON_ERROR(rmt_enable(led_chan), TAG, "enable RMT gagal");

    rgb_led_set(0, 0, 0);
    return ESP_OK;
}

void rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    /* WS2812 menerima urutan GRB. */
    uint8_t pixel[3] = { g, r, b };
    rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    if (rmt_transmit(led_chan, led_encoder, pixel, sizeof(pixel), &tx_cfg) == ESP_OK) {
        rmt_tx_wait_all_done(led_chan, 100);
    }
}
