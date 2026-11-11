#pragma once

// Clara's face on the AMOLED, from faces/clara in the talker repo: her textured eyes
// (iris over a dark sclera, lids), rendered on the host into layers, and six
// mouth shapes. Everything that draws runs inside the LVGL task on a 20 ms timer:
// the mouth follows the three viseme slots the talker satellite server rides on each
// audio frame (SetFaceFrame only records them), the gaze wanders, the eyes blink.

#include "display/lcd_display.h"
#include <atomic>
#include <functional>
#include <string_view>

class ClaraFaceDisplay : public SpiLcdDisplay {
public:
    ClaraFaceDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy);

    void SetupUI() override;
    bool SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetTheme(Theme* theme) override;       // the text font arrives with the theme
    void SetFaceFrame(uint32_t packed) override;      // audio task: records, never draws
    void SetPowerSaveMode(bool on) override;          // battery idle: panel asleep, ears stay on
    // A tap while the panel sleeps on battery: the board hands it to its power-save timer,
    // which brings the face back and restarts the idle countdown (nothing is bypassed).
    void OnTouchWake(std::function<void()> cb) { on_touch_wake_ = cb; }

private:
    struct Eye {
        lv_obj_t* box = nullptr;          // circular, clips the iris
        lv_obj_t* sclera = nullptr;
        lv_obj_t* iris = nullptr;
        lv_obj_t* emo = nullptr;          // the emotion's lid cut, under the blink
        lv_obj_t* lid = nullptr;
        const lv_image_dsc_t* const* emos = nullptr;
        const lv_image_dsc_t* const* lids = nullptr;
        int cx = 0, cy = 0;               // centre on the panel
        int iris_x0 = 0, iris_y0 = 0;     // iris at rest inside the box
        int shown_dx = 0, shown_dy = 0;
        int lid_level = 0;
        int emo_shown = 0;
    };

    static void FaceTimer(lv_timer_t* timer);
    void Tick();
    bool BecameReady();
    static void TouchEvent(lv_event_t* e);
    void OnTouch(lv_event_code_t code, int x, int y);
    void Tickled(int64_t now);
    void Say(const std::string_view& clip, int64_t& last_us);
    void PanelSleep(bool asleep);
    void GoToSleep();
    void WakeFromSleep(const char* why);
    void BuildEye(Eye& eye, lv_obj_t* screen, const lv_image_dsc_t* sclera,
                  const lv_image_dsc_t* iris, const lv_image_dsc_t* const* lids,
                  const lv_image_dsc_t* const* emos);
    void ApplyMouth(int viseme);
    void ApplyLid(Eye& eye, int level);
    void ApplyGaze(Eye& eye, int dx, int dy);
    void PlaceEyes();

    Eye eyes_[2];
    lv_obj_t* mouth_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_timer_t* face_timer_ = nullptr;

    // mouth: the frame now playing, written by the audio task
    std::atomic<uint32_t> frame_packed_{0};
    std::atomic<int64_t> frame_at_us_{0};
    int mouth_now_ = -1;

    // gaze (fractions of the eye radius) and its easing
    float gaze_x_ = 0, gaze_y_ = 0, target_x_ = 0, target_y_ = 0;
    int64_t next_gaze_us_ = 0;

    // blink: a sequence of lid levels stepped every tick
    int blink_step_ = -1;
    int64_t next_blink_us_ = 0;

    std::atomic<int> emotion_dy_{0};
    int shown_emotion_dy_ = 0;
    std::atomic<int> emotion_{0};              // index into the lid-cut table

    // Eyes closed from power-on until the wake word is listening; then they open once.
    bool ready_ = false;
    int64_t listening_since_us_ = 0;

    // touch (LVGL task only): a tap is a blink and a look, a hold a squint, a rub or
    // three quick taps a laugh
    static const int kTapHistory = 4;
    int64_t tap_us_[kTapHistory] = {0, 0, 0, 0};
    int tap_next_ = 0;
    int64_t press_start_us_ = 0;
    int last_touch_x_ = 0, last_touch_y_ = 0;
    int travel_px_ = 0;
    bool held_ = false;
    int64_t look_until_us_ = 0;
    float look_x_ = 0, look_y_ = 0;
    int64_t laugh_until_us_ = 0;
    int64_t boop_until_us_ = 0;
    int64_t last_hm_us_ = 0, last_mmm_us_ = 0, last_giggle_us_ = 0;
    bool swallow_press_ = false;               // the press that woke her, or put her to sleep
    bool sleeping_ = false;                    // put to sleep by hand (a long hold)
    int64_t sleep_at_us_ = 0;
    bool panel_asleep_ = false;
    std::function<void()> on_touch_wake_;
    bool drowsy_ = false;                      // lids parted, not yet open
};
