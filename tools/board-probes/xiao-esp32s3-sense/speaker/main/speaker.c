// XIAO ESP32S3 Sense + MAX98357A: speaker bring-up.
// At reset: a soft chime, then a 440 Hz tone at -30, -20, -12 and -6 dBFS (1 s each).
// Then it plays any 16 kHz 16-bit mono audio sent over USB as "PCM!"+len+samples (play.py).
// Wiring: BCLK = D0 (GPIO1), LRC = D1 (GPIO2), DIN = D3 (GPIO4), VIN = 5V, GND = GND.
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"

#define SPK_BCLK 1
#define SPK_LRC 2
#define SPK_DIN 4
#define RATE 16000

static i2s_chan_handle_t tx;

static void say(const char *t) {
    uint32_t n = strlen(t); uint8_t h[8] = {'T', 'X', 'T', '!'}; memcpy(h + 4, &n, 4);
    usb_serial_jtag_write_bytes(h, 8, portMAX_DELAY); usb_serial_jtag_write_bytes(t, n, portMAX_DELAY);
}

static void tone(float hz, float dbfs, float secs, float fade) {
    static int16_t b[320];
    float amp = 32767.f * powf(10.f, dbfs / 20.f), ph = 0, step = 2.f * (float)M_PI * hz / RATE;
    int total = (int)(secs * RATE), done = 0, nf = (int)(fade * RATE);
    while (done < total) {
        int n = total - done < 320 ? total - done : 320;
        for (int i = 0; i < n; i++, done++) {
            float env = 1.f;
            if (done < nf) env = (float)done / nf;
            if (total - done < nf) env = (float)(total - done) / nf;
            b[i] = (int16_t)(amp * env * sinf(ph)); ph += step; if (ph > 2.f * (float)M_PI) ph -= 2.f * (float)M_PI;
        }
        size_t w; i2s_channel_write(tx, b, n * 2, &w, portMAX_DELAY);
    }
}

void app_main(void) {
    usb_serial_jtag_driver_config_t uc = {.tx_buffer_size = 4096, .rx_buffer_size = 16384};
    usb_serial_jtag_driver_install(&uc);
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;                                  // silence, not a buzz, when nothing is queued
    i2s_new_channel(&cc, &tx, NULL);
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = SPK_BCLK, .ws = SPK_LRC, .dout = SPK_DIN, .din = I2S_GPIO_UNUSED},
    };
    i2s_channel_init_std_mode(tx, &sc);
    i2s_channel_enable(tx);
    vTaskDelay(pdMS_TO_TICKS(800));

    say("chime");
    tone(660, -18, 0.18f, 0.02f); tone(880, -18, 0.30f, 0.05f);
    vTaskDelay(pdMS_TO_TICKS(600));
    const float steps[] = {-30, -20, -12, -6};
    for (int i = 0; i < 4; i++) {
        char t[40]; snprintf(t, sizeof t, "tone 440 Hz at %.0f dBFS", steps[i]); say(t);
        tone(440, steps[i], 1.0f, 0.03f);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    say("ready for audio over USB");

    // "PCM!" + len + 16-bit mono samples at 16 kHz
    static uint8_t buf[8192]; size_t have = 0;
    while (1) {
        int n = usb_serial_jtag_read_bytes(buf + have, sizeof buf - have, pdMS_TO_TICKS(20));
        if (n > 0) have += n;
        while (have >= 8) {
            uint8_t *m = memmem(buf, have, "PCM!", 4);
            if (!m) { memmove(buf, buf + have - 3, 3); have = 3; break; }
            if (m != buf) { have -= m - buf; memmove(buf, m, have); }
            if (have < 8) break;
            uint32_t len; memcpy(&len, buf + 4, 4);
            if (len > sizeof buf - 8 || (len & 1)) { memmove(buf, buf + 4, have - 4); have -= 4; continue; }
            if (have < 8 + len) break;
            size_t w; i2s_channel_write(tx, buf + 8, len, &w, portMAX_DELAY);
            have -= 8 + len; memmove(buf, buf + 8 + len, have);
        }
    }
}
