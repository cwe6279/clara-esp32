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

#endif  // _BOARD_CONFIG_H_
