// XIAO ESP32S3 Sense: PDM microphone (16 kHz, 16-bit mono) -> 100 Hz high-pass -> fixed gain ->
// USB as "PCM!"+len+samples. Send one byte 0..60 over USB to set the gain in dB (default 30);
// the board answers "TXT!"+len+"gain N dB".
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_pdm.h"
#include "driver/usb_serial_jtag.h"

static void put(const char *magic, const void *data, uint32_t len) {
    uint8_t hdr[8]; memcpy(hdr, magic, 4); memcpy(hdr + 4, &len, 4);
    usb_serial_jtag_write_bytes(hdr, 8, portMAX_DELAY);
    usb_serial_jtag_write_bytes(data, len, portMAX_DELAY);
}

void app_main(void) {
    usb_serial_jtag_driver_config_t uc = {.tx_buffer_size = 16384, .rx_buffer_size = 256};
    usb_serial_jtag_driver_install(&uc);
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&cc, NULL, &rx);
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = 42, .din = 41, .invert_flags = {.clk_inv = false}},
    };
    i2s_channel_init_pdm_rx_mode(rx, &pc);
    i2s_channel_enable(rx);

    // 2nd-order Butterworth high-pass at 100 Hz (RBJ biquad)
    const float fs = 16000.f, fc = 100.f, q = 0.7071f;
    float w0 = 2.f * (float)M_PI * fc / fs, cw = cosf(w0), al = sinf(w0) / (2.f * q), a0 = 1.f + al;
    const float b0 = (1.f + cw) / 2.f / a0, b1 = -(1.f + cw) / a0, b2 = b0, a1 = -2.f * cw / a0, a2 = (1.f - al) / a0;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    int gain_db = 30; float gain = powf(10.f, gain_db / 20.f);

    static int16_t s[320];   // 20 ms
    while (1) {
        uint8_t cmd;
        if (usb_serial_jtag_read_bytes(&cmd, 1, 0) == 1 && cmd <= 60) {
            gain_db = cmd; gain = powf(10.f, gain_db / 20.f);
            char t[24]; int n = snprintf(t, sizeof t, "gain %d dB", gain_db); put("TXT!", t, n);
        }
        size_t got = 0;
        i2s_channel_read(rx, s, sizeof s, &got, portMAX_DELAY);
        for (int i = 0; i < (int)(got / 2); i++) {
            float x = s[i];
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            float v = y * gain;
            s[i] = v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int16_t)v;
        }
        put("PCM!", s, got);
    }
}
