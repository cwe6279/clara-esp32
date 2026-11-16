#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// Seeed XIAO ESP32S3 Sense, headless: PDM mic on the Sense board, MAX98357A amplifier on the
// header. Measurements behind these values: tools/board-probes/xiao-esp32s3-sense/README.md.

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// PDM microphone (on the Sense expansion board)
#define AUDIO_MIC_PDM_GPIO_CLK  GPIO_NUM_42
#define AUDIO_MIC_PDM_GPIO_DATA GPIO_NUM_41
// Fixed mic gain after a 100 Hz high-pass: 30 dB puts speech at 30 cm near -26 dBFS rms with
// peaks 3 dB short of clipping, and 4 m speech near -36 dBFS.
#define AUDIO_MIC_GAIN_DB       30.0f
#define AUDIO_MIC_HIGHPASS_HZ   100.0f

// MAX98357A: BCLK D0, LRC D1, DIN D3 (D2 is a strapping pin; D8..D10 belong to microSD)
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_1
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_2
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_4

// 1: also send her speech over the USB cable (usb_speaker.py plays it on the computer)
#define XIAO_USB_SPEAKER        1

#define BUILTIN_LED_GPIO        GPIO_NUM_21   // user LED, lit when low (shared with microSD CS)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// OV3660 on the Sense board (esp32-camera). 1600x1200 JPEG: the full 4:3 view of the sensor,
// judged clearly better than 1280x720 for describing a scene (see the board-probes README).
#define CAMERA_PIN_PWDN   GPIO_NUM_NC
#define CAMERA_PIN_RESET  GPIO_NUM_NC
#define CAMERA_PIN_XCLK   GPIO_NUM_10
#define CAMERA_PIN_SIOD   GPIO_NUM_40
#define CAMERA_PIN_SIOC   GPIO_NUM_39
#define CAMERA_PIN_D7     GPIO_NUM_48
#define CAMERA_PIN_D6     GPIO_NUM_11
#define CAMERA_PIN_D5     GPIO_NUM_12
#define CAMERA_PIN_D4     GPIO_NUM_14
#define CAMERA_PIN_D3     GPIO_NUM_16
#define CAMERA_PIN_D2     GPIO_NUM_18
#define CAMERA_PIN_D1     GPIO_NUM_17
#define CAMERA_PIN_D0     GPIO_NUM_15
#define CAMERA_PIN_VSYNC  GPIO_NUM_38
#define CAMERA_PIN_HREF   GPIO_NUM_47
#define CAMERA_PIN_PCLK   GPIO_NUM_13
#define CAMERA_XCLK_HZ    20000000
#define CAMERA_FRAME_SIZE FRAMESIZE_UXGA     // 1600x1200
#define CAMERA_JPEG_QUALITY 12

#endif  // _BOARD_CONFIG_H_
