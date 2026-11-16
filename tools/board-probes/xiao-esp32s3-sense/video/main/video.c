// XIAO ESP32S3 Sense (OV3660): measure the camera's own frame rate at several sizes, then stream
// JPEG frames over USB. Every record is "FRM!"+len+jpeg or "TXT!"+len+text, so a reader can resync.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "driver/usb_serial_jtag.h"

static void put(const char *magic, const void *data, uint32_t len) {
    uint8_t hdr[8]; memcpy(hdr, magic, 4); memcpy(hdr + 4, &len, 4);
    usb_serial_jtag_write_bytes(hdr, 8, portMAX_DELAY);
    const uint8_t *p = data;
    while (len) { uint32_t n = len > 4096 ? 4096 : len; usb_serial_jtag_write_bytes(p, n, portMAX_DELAY); p += n; len -= n; }
}
static void say(const char *fmt, ...) {
    char b[200]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b, fmt, a); va_end(a);
    put("TXT!", b, n);
}

void app_main(void) {
    usb_serial_jtag_driver_config_t uc = {.tx_buffer_size = 16384, .rx_buffer_size = 256};
    usb_serial_jtag_driver_install(&uc);
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10, .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14, .pin_d3 = 16, .pin_d2 = 18,
        .pin_d1 = 17, .pin_d0 = 15, .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_QXGA, .jpeg_quality = 12,
        .fb_count = 2, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (esp_camera_init(&c) != ESP_OK) { say("camera init failed"); return; }
    sensor_t *s = esp_camera_sensor_get();
    struct { framesize_t f; const char *name; } sizes[] = {
        {FRAMESIZE_QVGA, "320x240"}, {FRAMESIZE_VGA, "640x480"}, {FRAMESIZE_SVGA, "800x600"},
        {FRAMESIZE_HD, "1280x720"}, {FRAMESIZE_UXGA, "1600x1200"}, {FRAMESIZE_QXGA, "2048x1536"}};
    for (int i = 0; i < 0; i++) {
        s->set_framesize(s, sizes[i].f);
        for (int k = 0; k < 4; k++) { camera_fb_t *fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }
        int n = 0; uint64_t bytes = 0; int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 3000000) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) { n++; bytes += fb->len; esp_camera_fb_return(fb); }
        }
        double secs = (esp_timer_get_time() - t0) / 1e6;
        say("BENCH %s: camera delivers %.1f fps, frames average %u KB", sizes[i].name, n / secs, n ? (unsigned)(bytes / n / 1024) : 0);
    }
    s->set_framesize(s, FRAMESIZE_QXGA);
    say("STREAM 2048x1536 starts");
    int n = 0; int64_t t0 = esp_timer_get_time();
    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        uint8_t cmd;
        if (usb_serial_jtag_read_bytes(&cmd, 1, 0) == 1 && cmd >= '0' && cmd <= '5') {
            int k = cmd - '0';
            if (fb) esp_camera_fb_return(fb);
            s->set_framesize(s, sizes[k].f);
            say("STREAM %s", sizes[k].name);
            n = 0; t0 = esp_timer_get_time();
            continue;
        }
        if (!fb) continue;
        put("FRM!", fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (++n == 60) {
            say("board sent %.1f fps over USB", n / ((esp_timer_get_time() - t0) / 1e6));
            n = 0; t0 = esp_timer_get_time();
        }
    }
}
