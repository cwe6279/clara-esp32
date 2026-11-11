#include "clara_face_display.h"
#include "application.h"
#include "board.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cmath>
#include <cstring>

#define TAG "ClaraFace"

extern "C" {
extern const lv_image_dsc_t clara_mouth_sil, clara_mouth_pp, clara_mouth_ee,
                            clara_mouth_ah, clara_mouth_aa, clara_mouth_oo;
extern const lv_image_dsc_t clara_left_sclera, clara_left_iris,
                            clara_left_lid0, clara_left_lid1, clara_left_lid2, clara_left_lid3, clara_left_lid4;
extern const lv_image_dsc_t clara_right_sclera, clara_right_iris,
                            clara_right_lid0, clara_right_lid1, clara_right_lid2, clara_right_lid3, clara_right_lid4;
extern const lv_image_dsc_t clara_left_emo_happy, clara_left_emo_angry, clara_left_emo_annoyed, clara_left_emo_sad;
extern const lv_image_dsc_t clara_right_emo_happy, clara_right_emo_angry, clara_right_emo_annoyed, clara_right_emo_sad;
}

// Lid cuts per emotion, the same shapes as EMOTION_LIDS in the Python face.
// Index 0 is "no cut" (neutral, surprised); the server's names map in SetEmotion.
static const lv_image_dsc_t* kLeftEmos[] = {nullptr, &clara_left_emo_happy, &clara_left_emo_angry,
                                            &clara_left_emo_annoyed, &clara_left_emo_sad};
static const lv_image_dsc_t* kRightEmos[] = {nullptr, &clara_right_emo_happy, &clara_right_emo_angry,
                                             &clara_right_emo_annoyed, &clara_right_emo_sad};
static const int kEmoCount = 5;

// Viseme ids on the wire (talker/satellite.py): 0 sil, 1 pp, 2 ee, 3 ah, 4 aa, 5 oo.
static const lv_image_dsc_t* kMouths[] = {&clara_mouth_sil, &clara_mouth_pp, &clara_mouth_ee,
                                          &clara_mouth_ah,  &clara_mouth_aa, &clara_mouth_oo};
static const int kMouthCount = sizeof(kMouths) / sizeof(kMouths[0]);
struct Place { int x, y; };
// faces/clara/face.json scaled 0.6 onto the 368x448 panel (docs/SATELLITE_ESP32.md)
static const Place kMouthPos[] = {{141, 289}, {146, 290}, {129, 285}, {133, 277}, {129, 270}, {144, 278}};

static const lv_image_dsc_t* kLeftLids[] = {&clara_left_lid0, &clara_left_lid1, &clara_left_lid2,
                                            &clara_left_lid3, &clara_left_lid4};
static const lv_image_dsc_t* kRightLids[] = {&clara_right_lid0, &clara_right_lid1, &clara_right_lid2,
                                             &clara_right_lid3, &clara_right_lid4};
static const int kLidLevels = 5;
static const int kEyeSize = 114;                      // textured_eye size 190 * 0.6
static const int kIrisRest = 23;                      // iris image offset inside the eye box
static const int kMaxShift = 51;                      // 0.45 of the eye size, as on the host
static const float kGazeRadius = 0.35f;               // face.json gaze_radius
static const Place kEyeCentre[2] = {{87, 188}, {281, 188}};
static const int kBlinkSteps[] = {1, 2, 3, 4, 3, 2, 1, 0};
static const int kTickMs = 20;
static const int kListenSettleMs = CONFIG_CLARA_LISTEN_SETTLE_MS;   // mic open to eyes open

ClaraFaceDisplay::ClaraFaceDisplay(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle,
                                   int width, int height, int offset_x, int offset_y,
                                   bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {}

void ClaraFaceDisplay::BuildEye(Eye& eye, lv_obj_t* screen,
                                const lv_image_dsc_t* sclera, const lv_image_dsc_t* iris,
                                const lv_image_dsc_t* const* lids, const lv_image_dsc_t* const* emos) {
    eye.lids = lids;
    eye.emos = emos;
    eye.box = lv_obj_create(screen);
    lv_obj_remove_style_all(eye.box);
    lv_obj_set_size(eye.box, kEyeSize, kEyeSize);     // the lid overlay masks outside the eye
    lv_obj_set_style_bg_opa(eye.box, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(eye.box, LV_OBJ_FLAG_SCROLLABLE);

    eye.sclera = lv_image_create(eye.box);
    lv_image_set_src(eye.sclera, sclera);
    lv_obj_set_pos(eye.sclera, 0, 0);

    eye.iris = lv_image_create(eye.box);
    lv_image_set_src(eye.iris, iris);
    eye.iris_x0 = eye.iris_y0 = kIrisRest;
    lv_obj_set_pos(eye.iris, eye.iris_x0, eye.iris_y0);

    // alpha-only masks: the colour comes from the recolor style
    eye.emo = lv_image_create(eye.box);
    lv_obj_set_style_image_recolor(eye.emo, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(eye.emo, LV_OPA_COVER, 0);
    lv_obj_set_pos(eye.emo, 0, 0);
    lv_obj_add_flag(eye.emo, LV_OBJ_FLAG_HIDDEN);
    eye.emo_shown = 0;

    eye.lid = lv_image_create(eye.box);
    lv_obj_set_style_image_recolor(eye.lid, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(eye.lid, LV_OPA_COVER, 0);
    lv_image_set_src(eye.lid, lids[0]);
    lv_obj_set_pos(eye.lid, 0, 0);
    eye.lid_level = 0;
}

void ClaraFaceDisplay::PlaceEyes() {
    for (auto& eye : eyes_) {
        int cy = eye.cy + shown_emotion_dy_;
        lv_obj_set_pos(eye.box, eye.cx - kEyeSize / 2, cy - kEyeSize / 2);
    }
}

void ClaraFaceDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();                  // status bar, notifications, the usual objects

    DisplayLockGuard lock(this);
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // The chat layout and the emoji go away; the status bar stays, on black.
    if (container_ != nullptr) lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    eyes_[0].cx = kEyeCentre[0].x; eyes_[0].cy = kEyeCentre[0].y;
    eyes_[1].cx = kEyeCentre[1].x; eyes_[1].cy = kEyeCentre[1].y;
    BuildEye(eyes_[0], screen, &clara_left_sclera, &clara_left_iris, kLeftLids, kLeftEmos);
    BuildEye(eyes_[1], screen, &clara_right_sclera, &clara_right_iris, kRightLids, kRightEmos);
    PlaceEyes();

    mouth_ = lv_image_create(screen);
    lv_image_set_src(mouth_, kMouths[0]);
    lv_obj_set_pos(mouth_, kMouthPos[0].x, kMouthPos[0].y);
    mouth_now_ = 0;

    subtitle_ = lv_label_create(screen);       // font: set by SetTheme once the assets are loaded
    lv_obj_set_style_text_color(subtitle_, lv_color_make(110, 160, 210), 0);
    lv_obj_set_style_text_align(subtitle_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(subtitle_, LV_HOR_RES - 24);
    lv_obj_align(subtitle_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(subtitle_, "");

    if (status_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.1, 0);
        lv_obj_move_foreground(status_bar_);
    }

    // This AMOLED wants partial updates on even columns and rows, or they land a pixel off.
    lv_display_add_event_cb(lv_display_get_default(), [](lv_event_t* e) {
        auto area = static_cast<lv_area_t*>(lv_event_get_param(e));
        area->x1 &= ~1; area->y1 &= ~1;
        area->x2 |= 1;  area->y2 |= 1;
    }, LV_EVENT_INVALIDATE_AREA, nullptr);

    int64_t now = esp_timer_get_time();
    next_gaze_us_ = now + 2000 * 1000;
    next_blink_us_ = now + 2500 * 1000;
    face_timer_ = lv_timer_create(&ClaraFaceDisplay::FaceTimer, kTickMs, this);
    ESP_LOGI(TAG, "Clara's face is up");
}

// ── per-tick animation, inside the LVGL task ─────────────────────────────────
void ClaraFaceDisplay::FaceTimer(lv_timer_t* timer) {
    static_cast<ClaraFaceDisplay*>(lv_timer_get_user_data(timer))->Tick();
}

void ClaraFaceDisplay::ApplyMouth(int viseme) {
    if (viseme < 0 || viseme >= kMouthCount || viseme == mouth_now_) return;
    lv_image_set_src(mouth_, kMouths[viseme]);
    lv_obj_set_pos(mouth_, kMouthPos[viseme].x, kMouthPos[viseme].y);
    mouth_now_ = viseme;
}

void ClaraFaceDisplay::ApplyLid(Eye& eye, int level) {
    if (level < 0) level = 0;
    if (level >= kLidLevels) level = kLidLevels - 1;
    if (level == eye.lid_level) return;
    lv_image_set_src(eye.lid, eye.lids[level]);
    eye.lid_level = level;
}

void ClaraFaceDisplay::ApplyGaze(Eye& eye, int dx, int dy) {
    if (dx == eye.shown_dx && dy == eye.shown_dy) return;
    eye.shown_dx = dx; eye.shown_dy = dy;
    lv_obj_set_pos(eye.iris, eye.iris_x0 + dx, eye.iris_y0 + dy);
}

void ClaraFaceDisplay::SetPowerSaveMode(bool on) {
    // Idle on battery: the face stops and the panel is put to sleep (DISPOFF, then SLPIN,
    // which stops the driver's oscillator and charge pumps), while the wake word keeps
    // running; the power-save timer shuts the board down later. Waking is SLPOUT, the
    // driver's 120 ms settle, a full redraw, DISPON. The panel has no reset line, so the
    // driver's deep standby (a 1.3 s re-init on wake) never comes into it.
    DisplayLockGuard lock(this);
    if (on) {
        if (face_timer_) lv_timer_pause(face_timer_);
        if (panel_) {
            esp_lcd_panel_disp_on_off(panel_, false);
            esp_lcd_panel_disp_sleep(panel_, true);
        }
    } else {
        if (panel_) {
            esp_lcd_panel_disp_sleep(panel_, false);
            lv_obj_invalidate(lv_screen_active());
            lv_refr_now(lv_display_get_default());
            esp_lcd_panel_disp_on_off(panel_, true);
        }
        if (face_timer_) lv_timer_resume(face_timer_);
    }
}

// Before the wake word runs (Wi-Fi, the version check, loading the model) she is not
// listening, so the eyes stay shut; a face with open eyes means "say Clara".
bool ClaraFaceDisplay::BecameReady() {
    auto& app = Application::GetInstance();
    auto codec = Board::GetInstance().GetAudioCodec();
    if (app.GetDeviceState() != kDeviceStateIdle || !app.GetAudioService().IsWakeWordRunning() ||
        codec == nullptr || !codec->input_enabled()) {
        listening_since_us_ = 0;
        return false;
    }
    // The detector misses words said in its first seconds (every boot log puts the first
    // catch 3 to 5 s after the mic opens, whenever the name was said); the eyes stay shut
    // until it is worth saying her name.
    int64_t now = esp_timer_get_time();
    if (listening_since_us_ == 0) listening_since_us_ = now;
    if (now - listening_since_us_ < kListenSettleMs * 1000) return false;
    ready_ = true;
    blink_step_ = 4;                           // the second half of a blink: lids 3, 2, 1, 0
    ESP_LOGI(TAG, "Listening: eyes open");
    return true;
}

void ClaraFaceDisplay::Tick() {
    int64_t now = esp_timer_get_time();

    if (!ready_ && !BecameReady()) {
        ApplyMouth(0);
        for (auto& eye : eyes_) {
            ApplyGaze(eye, 0, 0);
            ApplyLid(eye, kLidLevels - 1);     // shut
        }
        return;
    }

    // mouth: which 20 ms slot of the frame now playing
    int64_t at = frame_at_us_.load();
    bool speaking = at != 0 && now - at < 250 * 1000;
    if (speaking) {
        int slot = (int)((now - at) / (20 * 1000));
        if (slot > 2) slot = 2;
        uint32_t packed = frame_packed_.load();
        ApplyMouth((packed >> (16 - 8 * slot)) & 0xff);
    } else {
        ApplyMouth(0);
    }

    // gaze: a new target now and then, eased toward; calmer while speaking
    if (now >= next_gaze_us_) {
        float amount = speaking ? kGazeRadius * 0.25f : kGazeRadius;
        float ang = (esp_random() % 6283) / 1000.0f;
        float rad = amount * sqrtf((esp_random() % 1000) / 1000.0f);
        target_x_ = rad * cosf(ang);
        target_y_ = rad * sinf(ang) * 0.6f;
        next_gaze_us_ = now + (int64_t)(2500 + esp_random() % 4500) * 1000;
    }
    gaze_x_ += (target_x_ - gaze_x_) * 0.12f;
    gaze_y_ += (target_y_ - gaze_y_) * 0.12f;
    int dx = (int)lroundf(gaze_x_ * kEyeSize * 0.5f);
    int dy = (int)lroundf(gaze_y_ * kEyeSize * 0.5f);
    if (dx > kMaxShift) dx = kMaxShift;
    if (dx < -kMaxShift) dx = -kMaxShift;
    if (dy > kMaxShift) dy = kMaxShift;
    if (dy < -kMaxShift) dy = -kMaxShift;

    // blink: step through the lid levels, then wait a few seconds
    int lid = 0;
    if (blink_step_ >= 0) {
        lid = kBlinkSteps[blink_step_++];
        if (blink_step_ >= (int)(sizeof(kBlinkSteps) / sizeof(kBlinkSteps[0]))) {
            blink_step_ = -1;
            next_blink_us_ = now + (int64_t)(3000 + esp_random() % 4000) * 1000;
        }
    } else if (now >= next_blink_us_) {
        blink_step_ = 0;
    }

    int edy = emotion_dy_.load();
    if (edy != shown_emotion_dy_) {
        shown_emotion_dy_ = edy;
        PlaceEyes();
    }
    int emo = emotion_.load();
    for (auto& eye : eyes_) {
        ApplyGaze(eye, dx, dy);
        ApplyLid(eye, lid);
        if (emo != eye.emo_shown) {
            eye.emo_shown = emo;
            if (emo > 0 && emo < kEmoCount && eye.emos[emo] != nullptr) {
                lv_image_set_src(eye.emo, eye.emos[emo]);
                lv_obj_remove_flag(eye.emo, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(eye.emo, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

// ── events from the rest of the firmware ─────────────────────────────────────
bool ClaraFaceDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return false;
    int dy = 0, emo = 0;
    if (strcmp(emotion, "sad") == 0) { dy = 10; emo = 4; }
    else if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0) { dy = -6; emo = 1; }
    else if (strcmp(emotion, "angry") == 0) emo = 2;
    else if (strcmp(emotion, "confused") == 0) emo = 3;          // talker's "annoyed"
    else if (strcmp(emotion, "surprised") == 0) dy = -12;
    emotion_dy_.store(dy);
    emotion_.store(emo);
    if (strcmp(emotion, "neutral") == 0) {
        frame_at_us_.store(0);                 // idle / listening: lips rest
    }
    return true;
}

void ClaraFaceDisplay::SetChatMessage(const char* role, const char* content) {
#if !CONFIG_CLARA_FACE_SUBTITLES
    return;                                    // the face speaks for itself
#endif
    if (subtitle_ == nullptr || content == nullptr) return;
    DisplayLockGuard lock(this);
    if (hide_subtitle_) {
        lv_label_set_text(subtitle_, "");
        return;
    }
    bool mine = role != nullptr && strcmp(role, "assistant") == 0;
    lv_obj_set_style_text_color(subtitle_, mine ? lv_color_make(110, 160, 210) : lv_color_make(120, 120, 120), 0);
    lv_label_set_text(subtitle_, content);
}

void ClaraFaceDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    if (subtitle_ == nullptr || lvgl_theme == nullptr || !lvgl_theme->text_font()) return;
    DisplayLockGuard lock(this);
    lv_obj_set_style_text_font(subtitle_, lvgl_theme->text_font()->font(), 0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);     // whatever the theme says
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    if (status_bar_ != nullptr) lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
}

void ClaraFaceDisplay::ClearChatMessages() {
    if (subtitle_ == nullptr) return;
    DisplayLockGuard lock(this);
    lv_label_set_text(subtitle_, "");
}

// Audio output task, as the frame is handed to the codec: record only. The LVGL
// timer applies the slots; a blocked lock here would starve the audio.
void ClaraFaceDisplay::SetFaceFrame(uint32_t packed) {
    frame_packed_.store(packed);
    frame_at_us_.store(esp_timer_get_time());
}
