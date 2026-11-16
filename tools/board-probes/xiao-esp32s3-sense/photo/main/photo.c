// Takes a full-resolution JPEG on the XIAO ESP32S3 Sense (OV3660) every time it is reset and
// prints it as base64 between markers on the USB serial console.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

void app_main(void) {
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10, .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14, .pin_d3 = 16, .pin_d2 = 18,
        .pin_d1 = 17, .pin_d0 = 15, .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_QXGA, .jpeg_quality = 8,
        .fb_count = 2, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_err_t e = esp_camera_init(&c);
    if (e != ESP_OK) { printf("CAMERA INIT FAILED: %s\n", esp_err_to_name(e)); return; }
    sensor_t *s = esp_camera_sensor_get();
    printf("sensor PID 0x%04x\n", s->id.PID);
    camera_fb_t *fb = NULL;
    for (int i = 0; i < 12; i++) {           // let exposure and white balance settle
        fb = esp_camera_fb_get();
        if (fb && i < 11) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (!fb) { printf("CAPTURE FAILED\n"); return; }
    printf("frame %ux%u, %u bytes\n", fb->width, fb->height, (unsigned)fb->len);
    size_t olen = 0;
    mbedtls_base64_encode(NULL, 0, &olen, fb->buf, fb->len);
    unsigned char *out = heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM);
    mbedtls_base64_encode(out, olen + 1, &olen, fb->buf, fb->len);
    printf("-----BEGIN JPEG-----\n"); fflush(stdout);
    for (size_t i = 0; i < olen; i += 512) {
        fwrite(out + i, 1, (olen - i) < 512 ? (olen - i) : 512, stdout);
        fflush(stdout);
    }
    printf("\n-----END JPEG-----\n"); fflush(stdout);
    esp_camera_fb_return(fb);
    while (1) vTaskDelay(portMAX_DELAY);
}
