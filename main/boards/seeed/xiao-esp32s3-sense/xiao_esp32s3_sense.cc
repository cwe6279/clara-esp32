// Seeed XIAO ESP32S3 Sense as a headless character: hears through the Sense board's PDM mic,
// speaks through a MAX98357A, shows its state on the user LED, no display. The camera is a
// later step (see tools/board-probes/xiao-esp32s3-sense/README.md).
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/gpio_led.h"

#include <cmath>
#include <cstring>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <esp_log.h>

#define TAG "XiaoEsp32S3Sense"

// The PDM mic is quiet and full of rumble (86 % of its energy below 100 Hz). A 2nd-order
// Butterworth high-pass, then a fixed gain, as calibrated with the board-probes mic project.
class XiaoSenseCodec : public NoAudioCodecSimplexPdm {
public:
    XiaoSenseCodec()
        : NoAudioCodecSimplexPdm(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                 AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
                                 I2S_STD_SLOT_BOTH,     // both slots: the amp mixes (L+R)/2 with SD open
                                 AUDIO_MIC_PDM_GPIO_CLK, AUDIO_MIC_PDM_GPIO_DATA) {
        const float w0 = 2.0f * (float)M_PI * AUDIO_MIC_HIGHPASS_HZ / AUDIO_INPUT_SAMPLE_RATE;
        const float cw = cosf(w0), al = sinf(w0) / (2.0f * 0.7071f), a0 = 1.0f + al;
        b0_ = (1.0f + cw) / 2.0f / a0; b1_ = -(1.0f + cw) / a0; b2_ = b0_;
        a1_ = -2.0f * cw / a0;          a2_ = (1.0f - al) / a0;
        gain_ = powf(10.0f, AUDIO_MIC_GAIN_DB / 20.0f);
        ESP_LOGI(TAG, "mic: %.0f Hz high-pass, %.0f dB gain", AUDIO_MIC_HIGHPASS_HZ, AUDIO_MIC_GAIN_DB);
#if XIAO_USB_SPEAKER
        // Until the amplifier is fitted: also send what she says over the USB cable as
        // "SPK!"+len+16-bit mono samples at AUDIO_OUTPUT_SAMPLE_RATE (tools/.../usb_speaker.py
        // plays it). The console keeps working through the same driver.
        usb_serial_jtag_driver_config_t uc = {.tx_buffer_size = 16384, .rx_buffer_size = 256};
        if (usb_serial_jtag_driver_install(&uc) == ESP_OK) {
            usb_serial_jtag_vfs_use_driver();
            ESP_LOGI(TAG, "speech also goes out over USB");
        }
#endif
    }

protected:
#if XIAO_USB_SPEAKER
    int Write(const int16_t* data, int samples) override {
        if (samples > 0) {
            uint8_t hdr[8] = {'S', 'P', 'K', '!'};
            uint32_t len = samples * sizeof(int16_t);
            memcpy(hdr + 4, &len, 4);
            // Never block: with nobody reading the cable, the frames are simply dropped.
            if (usb_serial_jtag_write_bytes(hdr, 8, 0) == 8) {
                usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(20));
            }
        }
        return NoAudioCodecSimplexPdm::Write(data, samples);   // the I2S amplifier, paced in real time
    }
#endif

    int Read(int16_t* dest, int samples) override {
        int n = NoAudioCodecSimplexPdm::Read(dest, samples);
        for (int i = 0; i < n; i++) {
            float x = dest[i];
            float y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
            x2_ = x1_; x1_ = x; y2_ = y1_; y1_ = y;
            float v = y * gain_;
            dest[i] = v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : (int16_t)v;
        }
        return n;
    }

private:
    float b0_, b1_, b2_, a1_, a2_, gain_;
    float x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

class XiaoEsp32S3Sense : public WifiBoard {
private:
    Button boot_button_;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    XiaoEsp32S3Sense() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeButtons();
    }

    Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 1);   // lit when the pin is low
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static XiaoSenseCodec codec;
        return &codec;
    }
};

DECLARE_BOARD(XiaoEsp32S3Sense);
