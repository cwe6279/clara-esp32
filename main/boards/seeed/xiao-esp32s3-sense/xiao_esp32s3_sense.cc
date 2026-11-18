// Seeed XIAO ESP32S3 Sense as a headless character: hears through the Sense board's PDM mic,
// speaks through a MAX98357A, sees through the OV3660 (photos on request, uploaded to the host),
// shows its state on the user LED, no display. See tools/board-probes/xiao-esp32s3-sense/README.md.
#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/gpio_led.h"
#include "esp32_camera.h"

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

// Status sounds (tools/status-sounds, embedded from assets/common/status_*.ogg): short original
// droid chirps that finish before listening opens, never during a voice note.
#define STATUS_SOUND(name) \
    extern const char status_##name##_start[] asm("_binary_status_" #name "_ogg_start"); \
    extern const char status_##name##_end[] asm("_binary_status_" #name "_ogg_end");
STATUS_SOUND(wake)
STATUS_SOUND(connected)
STATUS_SOUND(shutter)
STATUS_SOUND(listening_ends)

static void PlayStatus(const char* start, const char* end) {
    Application::GetInstance().GetAudioService().PlaySound(std::string_view(start, end - start));
}
#define PLAY_STATUS(name) PlayStatus(status_##name##_start, status_##name##_end)

// The one LED we control (amber, GPIO21) says what she is doing by pattern; the red one beside
// it is the charger's. Off while idle, so a lit LED always means something:
//   fast blink   starting, connecting, updating      slow blink   Wi-Fi setup hotspot
//   solid        listening                           breathing    speaking
//   double flash the camera's shutter
class XiaoSenseLed : public GpioLed {
public:
    XiaoSenseLed() : GpioLed(BUILTIN_LED_GPIO, 1) {}

    void OnStateChanged() override {
        auto state = Application::GetInstance().GetDeviceState();
        Chime(last_, state);
        last_ = state;
        switch (state) {
            case kDeviceStateStarting:
            case kDeviceStateActivating:
            case kDeviceStateConnecting:
            case kDeviceStateUpgrading:
                SetBrightness(100);
                StartContinuousBlink(120);
                break;
            case kDeviceStateWifiConfiguring:
                SetBrightness(100);
                StartContinuousBlink(600);
                break;
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                SetBrightness(100);
                TurnOn();
                break;
            case kDeviceStateSpeaking:
            case kDeviceStateNotifying:
                SetBrightness(100);
                StartFadeTask();
                break;
            case kDeviceStateIdle:
            default:
                TurnOff();
                break;
        }
    }

    void Shutter() {
        SetBrightness(100);
        Blink(2, 70);
        PLAY_STATUS(shutter);
    }

private:
    DeviceState last_ = kDeviceStateUnknown;
    bool greeted_ = false;

    // A sound for the moments the LED alone would leave unclear: ready after boot, heard her
    // name, done and back to idle.
    void Chime(DeviceState from, DeviceState to) {
        if (from == to) return;
        if (to == kDeviceStateIdle && !greeted_ &&
            (from == kDeviceStateActivating || from == kDeviceStateStarting)) {
            greeted_ = true;
            PLAY_STATUS(connected);
        } else if (from == kDeviceStateIdle && (to == kDeviceStateConnecting || to == kDeviceStateListening)) {
            PLAY_STATUS(wake);
        } else if (to == kDeviceStateIdle && greeted_ &&
                   (from == kDeviceStateListening || from == kDeviceStateSpeaking)) {
            PLAY_STATUS(listening_ends);
        }
    }
};

// The camera, with the LED's shutter flash when a photo is taken.
class XiaoSenseCamera : public Esp32Camera {
public:
    XiaoSenseCamera(const camera_config_t& config, XiaoSenseLed* led) : Esp32Camera(config), led_(led) {}

    // The sharpest of a few fresh frames. At one JPEG quality a blurred frame (a hand moving,
    // the board settling) compresses smaller, so the largest of CAMERA_BEST_OF is the sharpest.
    // Two frame buffers: one holds the best so far while the other takes the next; with the
    // grab-when-empty mode the camera then rests between photos.
    bool Capture() override {
        if (encoder_thread_.joinable()) {
            encoder_thread_.join();
        }
        if (!streaming_on_) {
            return false;
        }
        if (current_fb_) {
            esp_camera_fb_return(current_fb_);
            current_fb_ = nullptr;
        }
        for (int i = 0; i < 2; i++) {                       // both buffers hold old frames: drop them
            if (camera_fb_t* stale = esp_camera_fb_get()) esp_camera_fb_return(stale);
        }
        camera_fb_t* best = esp_camera_fb_get();
        if (!best) {
            ESP_LOGE(TAG, "camera capture failed");
            return false;
        }
        size_t sizes[CAMERA_BEST_OF] = {best->len};
        for (int i = 1; i < CAMERA_BEST_OF; i++) {
            camera_fb_t* next = esp_camera_fb_get();
            if (!next) break;
            sizes[i] = next->len;
            if (next->len > best->len) {
                esp_camera_fb_return(best);
                best = next;
            } else {
                esp_camera_fb_return(next);
            }
        }
        current_fb_ = best;
        ESP_LOGI(TAG, "photo %ux%u: kept %u bytes of %u / %u / %u", best->width, best->height,
                 (unsigned)best->len, (unsigned)sizes[0], (unsigned)sizes[1], (unsigned)sizes[2]);
        // The click and flash come AFTER the frame is in: a click at the start told people the
        // photo was taken while the shutter was still open, so they moved and it blurred.
        if (led_) led_->Shutter();
        return true;
    }

    std::expected<std::string, std::string> Explain(const std::string& question) override {
        auto result = Esp32Camera::Explain(question);
        if (led_) led_->OnStateChanged();          // back to the state's own pattern
        return result;
    }

private:
    XiaoSenseLed* led_;
};

class XiaoEsp32S3Sense : public WifiBoard {
private:
    Button boot_button_;
    Esp32Camera* camera_ = nullptr;

    static XiaoSenseLed* SenseLed() {
        static XiaoSenseLed led;
        return &led;
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = CAMERA_PIN_SIOD;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = CAMERA_XCLK_HZ;
        config.ledc_timer = LEDC_TIMER_2;          // the LED uses its own LEDC timer and channel
        config.ledc_channel = LEDC_CHANNEL_2;
        config.pixel_format = PIXFORMAT_JPEG;      // the sensor encodes; nothing to re-encode here
        config.frame_size = CAMERA_FRAME_SIZE;
        config.jpeg_quality = CAMERA_JPEG_QUALITY;
        config.fb_count = 2;                       // two: one keeps the best frame, one takes the next
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        camera_ = new XiaoSenseCamera(config, SenseLed());
        // The OV3660 on the Sense board comes up mirrored left to right (a label read backwards
        // and the vision model could not read text): flip it, whatever the driver's default was.
        if (sensor_t* sensor = esp_camera_sensor_get()) {
            int was = sensor->status.hmirror;
            sensor->set_hmirror(sensor, was ? 0 : 1);
            ESP_LOGI(TAG, "camera: mirror %d -> %d", was, was ? 0 : 1);
            // Dim rooms: let the sensor raise its gain further and not stretch the exposure
            // (night mode), so a photo is grainier but not smeared by a moving hand.
            sensor->set_gainceiling(sensor, GAINCEILING_64X);
            sensor->set_aec2(sensor, 0);
            // Indoors the photos came out one to two stops dark (an average near 45 of 255): aim the
            // auto-exposure one step brighter (-2..2). It uses a longer exposure or more gain as the
            // light allows, so the resolution is unchanged.
            sensor->set_ae_level(sensor, 1);
        }
    }

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
        InitializeCamera();
    }

    Camera* GetCamera() override {
        return camera_;
    }

    Led* GetLed() override {
        return SenseLed();                          // lit when the pin is low
    }

    AudioCodec* GetAudioCodec() override {
        static XiaoSenseCodec codec;
        return &codec;
    }
};

DECLARE_BOARD(XiaoEsp32S3Sense);
