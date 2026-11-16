// One-shot hardware probe for a Seeed XIAO ESP32S3 Sense: chip, memory, camera sensor ID,
// I2C devices, PDM microphone level, microSD. Prints a report every 10 s.
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_clk_tree.h"
#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "driver/i2s_pdm.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#define CAM_XCLK 10
#define CAM_SIOD 40
#define CAM_SIOC 39
#define MIC_CLK 42
#define MIC_DAT 41
#define SD_CS 21
#define SD_SCK 7
#define SD_MISO 8
#define SD_MOSI 9

static i2c_master_bus_handle_t bus;

static void xclk_on(void) {
    ledc_timer_config_t t = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
                             .timer_num = LEDC_TIMER_0, .freq_hz = 20000000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&t);
    ledc_channel_config_t c = {.gpio_num = CAM_XCLK, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0, .duty = 1, .hpoint = 0};
    ledc_channel_config(&c);
}

static i2c_master_dev_handle_t dev(uint8_t addr) {
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000};
    i2c_master_dev_handle_t h = NULL;
    i2c_master_bus_add_device(bus, &d, &h);
    return h;
}

static void camera(void) {
    xclk_on();
    vTaskDelay(pdMS_TO_TICKS(50));
    i2c_master_bus_config_t b = {.i2c_port = 0, .sda_io_num = CAM_SIOD, .scl_io_num = CAM_SIOC,
                                 .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                 .flags.enable_internal_pullup = true};
    if (i2c_new_master_bus(&b, &bus) != ESP_OK) { printf("camera bus: failed to open\n"); return; }
    printf("I2C devices on the camera bus:");
    int n = 0;
    for (int a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(bus, a, 50) == ESP_OK) { printf(" 0x%02x", a); n++; }
    printf(n ? "\n" : " none\n");
    if (i2c_master_probe(bus, 0x30, 50) == ESP_OK) {          // OV2640 family: 8-bit registers, bank select
        i2c_master_dev_handle_t h = dev(0x30);
        uint8_t sel[2] = {0xFF, 0x01}; i2c_master_transmit(h, sel, 2, 100);
        uint8_t r, pid = 0, ver = 0, midh = 0, midl = 0;
        r = 0x0A; i2c_master_transmit_receive(h, &r, 1, &pid, 1, 100);
        r = 0x0B; i2c_master_transmit_receive(h, &r, 1, &ver, 1, 100);
        r = 0x1C; i2c_master_transmit_receive(h, &r, 1, &midh, 1, 100);
        r = 0x1D; i2c_master_transmit_receive(h, &r, 1, &midl, 1, 100);
        printf("camera @0x30: PID 0x%02x VER 0x%02x MID 0x%02x%02x -> %s\n", pid, ver, midh, midl,
               pid == 0x26 ? "OV2640" : pid == 0x77 ? "OV7725" : pid == 0x76 ? "OV7670" : "unknown");
    }
    if (i2c_master_probe(bus, 0x3C, 50) == ESP_OK) {          // OV3660 / OV5640: 16-bit registers
        i2c_master_dev_handle_t h = dev(0x3C);
        uint8_t r1[2] = {0x30, 0x0A}, r2[2] = {0x30, 0x0B}, hi = 0, lo = 0;
        i2c_master_transmit_receive(h, r1, 2, &hi, 1, 100);
        i2c_master_transmit_receive(h, r2, 2, &lo, 1, 100);
        printf("camera @0x3C: chip ID 0x%02x%02x -> %s\n", hi, lo,
               (hi == 0x36 && lo == 0x60) ? "OV3660" : (hi == 0x56 && lo == 0x40) ? "OV5640" : "unknown");
    }
}

static void mic(void) {
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cc, NULL, &rx) != ESP_OK) { printf("mic: no channel\n"); return; }
    i2s_pdm_rx_config_t pc = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = MIC_CLK, .din = MIC_DAT, .invert_flags = {.clk_inv = false}},
    };
    i2s_channel_init_pdm_rx_mode(rx, &pc);
    i2s_channel_enable(rx);
    static int16_t s[16000];
    size_t got = 0, tot = 0;
    for (int i = 0; i < 4; i++) {   // throw away the first 0.25 s, then keep 1 s
        i2s_channel_read(rx, s, sizeof(int16_t) * 4000, &got, 1000);
    }
    while (tot < sizeof(s)) { i2s_channel_read(rx, (uint8_t *)s + tot, sizeof(s) - tot, &got, 1000); if (!got) break; tot += got; }
    int n = tot / 2, peak = 0; double sum = 0, mean = 0;
    for (int i = 0; i < n; i++) mean += s[i];
    mean /= n ? n : 1;
    for (int i = 0; i < n; i++) { double v = s[i] - mean; sum += v * v; if (abs(s[i]) > peak) peak = abs(s[i]); }
    double rms = n ? sqrt(sum / n) : 0;
    printf("mic (PDM, clk %d / data %d): %d samples, rms %.0f, peak %d, dBFS %.1f -> %s\n", MIC_CLK, MIC_DAT, n, rms, peak,
           rms > 0 ? 20 * log10(rms / 32768.0) : -999.0, (n && peak > 0 && rms > 1) ? "alive" : "silent (no mic?)");
    i2s_channel_disable(rx); i2s_del_channel(rx);
}

static void sd(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bc = {.mosi_io_num = SD_MOSI, .miso_io_num = SD_MISO, .sclk_io_num = SD_SCK,
                           .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000};
    if (spi_bus_initialize(host.slot, &bc, SDSPI_DEFAULT_DMA) != ESP_OK) { printf("sd: spi bus failed\n"); return; }
    sdspi_device_config_t dc = SDSPI_DEVICE_CONFIG_DEFAULT();
    dc.gpio_cs = SD_CS; dc.host_id = host.slot;
    int h; if (sdspi_host_init() != ESP_OK || sdspi_host_init_device(&dc, &h) != ESP_OK) { printf("sd: host init failed\n"); return; }
    host.slot = h;
    sdmmc_card_t card;
    esp_err_t e = sdmmc_card_init(&host, &card);
    if (e == ESP_OK) printf("microSD: card present, %llu MB, name %s\n",
                            ((unsigned long long)card.csd.capacity) * card.csd.sector_size / (1024 * 1024), card.cid.name);
    else printf("microSD: slot answers no card (%s)\n", esp_err_to_name(e));
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_chip_info_t ci; esp_chip_info(&ci);
    uint32_t fs = 0; esp_flash_get_size(NULL, &fs);
    uint32_t cpu = 0; esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_EXACT, &cpu);
    printf("\n==== XIAO ESP32S3 Sense probe ====\n");
    printf("chip: ESP32-S3 rev %d.%d, %d cores, %lu MHz\n", ci.revision / 100, ci.revision % 100, ci.cores, cpu / 1000000);
    printf("flash: %lu MB; PSRAM: %u KB (free %u KB); internal RAM free: %u KB\n", fs >> 20,
           (unsigned)(esp_psram_get_size() >> 10), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
    camera();
    mic();
    sd();
    printf("==== done ====\n");
    while (1) { vTaskDelay(pdMS_TO_TICKS(10000)); mic(); }
}
