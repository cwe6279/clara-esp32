#include "clara_face_display.h"
#include "application.h"
#include "board.h"
#include "assets/lang_config.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define TAG "ClaraFace"

#include "clara_face_layout.h"             // generated: talker tools/satellite_face_art.py
#include "clara_motion.h"

extern "C" {
extern const lv_image_dsc_t FACE_SYM(mouth_sil), FACE_SYM(mouth_pp), FACE_SYM(mouth_ee),
                            FACE_SYM(mouth_ah), FACE_SYM(mouth_aa), FACE_SYM(mouth_oo);
extern const lv_image_dsc_t FACE_SYM(left_sclera), FACE_SYM(left_iris),
                            FACE_SYM(left_lid0), FACE_SYM(left_lid1), FACE_SYM(left_lid2), FACE_SYM(left_lid3), FACE_SYM(left_lid4);
extern const lv_image_dsc_t FACE_SYM(right_sclera), FACE_SYM(right_iris),
                            FACE_SYM(right_lid0), FACE_SYM(right_lid1), FACE_SYM(right_lid2), FACE_SYM(right_lid3), FACE_SYM(right_lid4);
extern const lv_image_dsc_t FACE_SYM(left_emo_happy), FACE_SYM(left_emo_angry), FACE_SYM(left_emo_annoyed), FACE_SYM(left_emo_sad);
extern const lv_image_dsc_t FACE_SYM(right_emo_happy), FACE_SYM(right_emo_angry), FACE_SYM(right_emo_annoyed), FACE_SYM(right_emo_sad);
}

// Lid cuts per emotion, the same shapes as EMOTION_LIDS in the Python face.
// Index 0 is "no cut" (neutral, surprised); the server's names map in SetEmotion.
static const lv_image_dsc_t* kLeftEmos[] = {nullptr, &FACE_SYM(left_emo_happy), &FACE_SYM(left_emo_angry),
                                            &FACE_SYM(left_emo_annoyed), &FACE_SYM(left_emo_sad)};
static const lv_image_dsc_t* kRightEmos[] = {nullptr, &FACE_SYM(right_emo_happy), &FACE_SYM(right_emo_angry),
                                             &FACE_SYM(right_emo_annoyed), &FACE_SYM(right_emo_sad)};
static const int kEmoCount = 5;

// Viseme ids on the wire (talker/satellite.py): 0 sil, 1 pp, 2 ee, 3 ah, 4 aa, 5 oo.
static const lv_image_dsc_t* kMouths[] = {&FACE_SYM(mouth_sil), &FACE_SYM(mouth_pp), &FACE_SYM(mouth_ee),
                                          &FACE_SYM(mouth_ah),  &FACE_SYM(mouth_aa), &FACE_SYM(mouth_oo)};
static const int kMouthCount = sizeof(kMouths) / sizeof(kMouths[0]);
struct Place { int x, y; };
// positions from the generated layout header (face.json scaled onto the panel)
static const Place kMouthPos[] = FACE_MOUTH_POS;

static const lv_image_dsc_t* kLeftLids[] = {&FACE_SYM(left_lid0), &FACE_SYM(left_lid1), &FACE_SYM(left_lid2),
                                            &FACE_SYM(left_lid3), &FACE_SYM(left_lid4)};
static const lv_image_dsc_t* kRightLids[] = {&FACE_SYM(right_lid0), &FACE_SYM(right_lid1), &FACE_SYM(right_lid2),
                                             &FACE_SYM(right_lid3), &FACE_SYM(right_lid4)};
static const int kLidLevels = 5;
static const int kEyeSize = FACE_EYE_SIZE;
static const int kIrisRest = FACE_IRIS_REST;         // iris image offset inside the eye box
static const int kMaxShift = FACE_MAX_SHIFT;         // 0.45 of the eye size, as on the host
static const float kGazeRadius = FACE_GAZE_RADIUS;   // face.json gaze_radius
static const Place kEyeCentre[2] = {{FACE_EYE_L_CX, FACE_EYE_L_CY}, {FACE_EYE_R_CX, FACE_EYE_R_CY}};
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
    lv_obj_remove_flag(eye.box, LV_OBJ_FLAG_CLICKABLE);   // touches go to the screen

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
    BuildEye(eyes_[0], screen, &FACE_SYM(left_sclera), &FACE_SYM(left_iris), kLeftLids, kLeftEmos);
    BuildEye(eyes_[1], screen, &FACE_SYM(right_sclera), &FACE_SYM(right_iris), kRightLids, kRightEmos);
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

    // Touch: a tap gets a blink and a look; a hold a squint; taps in a row or a rub, a laugh.
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    for (auto ev : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED}) {
        lv_obj_add_event_cb(screen, &ClaraFaceDisplay::TouchEvent, ev, this);
    }

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

// ── touch, also inside the LVGL task ─────────────────────────────────────────
static const int kTickleTaps = 3;                      // taps within kTickleWindowMs
static const int kTickleWindowMs = 1500;
static const int kTickleTravelPx = 120;                // a finger rubbed this far in one press
static const int kHoldMs = 350;
static const int kSleepHoldMs = 2000;                  // hold this long: she goes to sleep
static const int kLaughMs = 2600;
static const int kBoopMs = 700;                        // wide eyes after a tap
static const int kLookMs = 1500;
static const int kSoundGapMs = 8000;                   // a given reaction sound at most this often
static const bool kComposed = CONFIG_CLARA_TOUCH_COMPOSED;

// A short clip from the board itself, right away, only while idle (in a conversation the
// host owns the audio and the brain gets the event instead), and not too often.
// The wake word is paused while the clip plays: the mic hears her own voice, and the
// first time she said "let's keep it professional" the detector took it for her name and
// opened a channel.
static const int kClipMuteMs = 3500;

void ClaraFaceDisplay::Say(const std::string_view& clip, int64_t& last_us) {
    int64_t now = esp_timer_get_time();
    if (now - last_us < (int64_t)kSoundGapMs * 1000) return;
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) return;
    last_us = now;
    ears_back_at_us_ = now + (int64_t)kClipMuteMs * 1000;
    app.Schedule([&app, clip]() {
        app.GetAudioService().EnableWakeWordDetection(false);
        app.PlaySound(clip);
    });
}

// Called each tick: once the clip is over, the ears come back (if she is still idle).
void ClaraFaceDisplay::EarsBack(int64_t now) {
    if (ears_back_at_us_ == 0 || now < ears_back_at_us_) return;
    ears_back_at_us_ = 0;
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        if (app.GetDeviceState() == kDeviceStateIdle) app.GetAudioService().EnableWakeWordDetection(true);
    });
}

void ClaraFaceDisplay::TouchEvent(lv_event_t* e) {
    auto self = static_cast<ClaraFaceDisplay*>(lv_event_get_user_data(e));
    lv_point_t p = {0, 0};
    if (auto indev = lv_event_get_indev(e)) lv_indev_get_point(indev, &p);
    self->OnTouch(lv_event_get_code(e), p.x, p.y);
}

void ClaraFaceDisplay::Tickled(int64_t now) {
    bool fresh = now >= laugh_until_us_;
    laugh_until_us_ = now + (int64_t)kLaughMs * 1000;
    if (!fresh) return;
    if (kComposed) Say(Lang::Sounds::OGG_PROFESSIONAL, last_professional_us_);   // a throat-clear: "Let's keep it professional."
    else Say(Lang::Sounds::OGG_GIGGLE, last_giggle_us_);
}

void ClaraFaceDisplay::OnTouch(lv_event_code_t code, int x, int y) {
    if (!ready_) return;                               // not up yet
    int64_t now = esp_timer_get_time();
    if (code != LV_EVENT_PRESSING) {
        ESP_LOGI(TAG, "touch %s at %d,%d", code == LV_EVENT_PRESSED ? "down" : "up", x, y);
    }
    if (sleeping_ || panel_asleep_) {                  // tap to wake; that press does nothing else
                                                       // (on battery the panel sleeps for real and
                                                       // the touch goes with it: the wake word wakes her)
        if (code == LV_EVENT_PRESSED) {
            swallow_press_ = true;
            if (sleeping_) WakeFromSleep("tapped");
            else if (on_touch_wake_) { ESP_LOGI(TAG, "tapped while asleep on battery"); on_touch_wake_(); }
        }
        return;
    }
    if (swallow_press_) {
        if (code == LV_EVENT_RELEASED) swallow_press_ = false;
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        press_start_us_ = now;
        travel_px_ = 0;
        last_touch_x_ = x; last_touch_y_ = y;
        // a tap: eyes wide (playful) or a blink (composed), then a look at your finger
        if (kComposed) { if (blink_step_ < 0) blink_step_ = 0; }
        else boop_until_us_ = now + (int64_t)kBoopMs * 1000;
        look_until_us_ = now + (int64_t)kLookMs * 1000;
        // taps in a row
        int64_t window = now - (int64_t)kTickleWindowMs * 1000;
        int n = 0;
        for (int i = 0; i < kTapHistory; i++) if (tap_us_[i] > window) n++;
        tap_us_[tap_next_++ % kTapHistory] = now;
        if (n + 1 >= kTickleTaps) {
            if (now >= laugh_until_us_) ESP_LOGI(TAG, "tickled: %d taps", n + 1);
            Tickled(now);
        } else {
            Say(Lang::Sounds::OGG_HM, last_hm_us_);
        }
    } else if (code == LV_EVENT_PRESSING) {
        travel_px_ += abs(x - last_touch_x_) + abs(y - last_touch_y_);
        last_touch_x_ = x; last_touch_y_ = y;
        look_until_us_ = now + (int64_t)kLookMs * 1000;
        bool held = now - press_start_us_ > (int64_t)kHoldMs * 1000;
        if (held && !held_ && !kComposed) Say(Lang::Sounds::OGG_MMM, last_mmm_us_);   // composed: the look says it
        held_ = held;
        if (now - press_start_us_ > (int64_t)kSleepHoldMs * 1000 && travel_px_ < kTickleTravelPx) {
            GoToSleep();
            swallow_press_ = true;
            return;
        }
        if (travel_px_ > kTickleTravelPx) {            // rubbed: that tickles
            if (now >= laugh_until_us_) ESP_LOGI(TAG, "tickled: rubbed");
            Tickled(now);
            travel_px_ = 0;
        }
    } else if (code == LV_EVENT_RELEASED) {
        held_ = false;
    }
    // where the finger is, as a gaze target: fractions of the eye radius
    float fx = (x - LV_HOR_RES / 2.0f) / (LV_HOR_RES / 2.0f);
    float fy = (y - LV_VER_RES / 2.0f) / (LV_VER_RES / 2.0f);
    look_x_ = fx * 0.45f; look_y_ = fy * 0.30f;
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

// The panel asleep is DISPOFF then SLPIN, which stops the driver's oscillator and charge
// pumps. Waking is SLPOUT, the driver's 120 ms settle, a full redraw, DISPON. The panel has
// no reset line, so the driver's deep standby (a 1.3 s re-init on wake) never comes into it.
void ClaraFaceDisplay::PanelSleep(bool asleep) {
    if (panel_ == nullptr || asleep == panel_asleep_) return;
    panel_asleep_ = asleep;
    if (asleep) {
        esp_lcd_panel_disp_on_off(panel_, false);
        esp_lcd_panel_disp_sleep(panel_, true);
    } else {
        esp_lcd_panel_disp_sleep(panel_, false);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(lv_display_get_default());
        esp_lcd_panel_disp_on_off(panel_, true);
    }
}

// Idle on battery (the power-save timer): the face stops and the panel sleeps while the
// wake word keeps running; the timer shuts the board down later. Waking (the channel
// opening, or USB) brings the face back, and ends a sleep she was put into by hand too.
void ClaraFaceDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (on) {
        if (face_timer_) lv_timer_pause(face_timer_);
        PanelSleep(true);
    } else {
        sleeping_ = false;
        PanelSleep(false);
        if (face_timer_) lv_timer_resume(face_timer_);
        blink_step_ = 5;                               // lids open
    }
}

// A long hold: she closes her eyes and the screen goes black; the ears stay on. A tap,
// the wake word, or USB wakes her. The panel itself stays awake: with it in SLPIN the
// touch controller reports nothing, so a tap could never wake her (an AMOLED showing
// black draws next to nothing anyway; the battery timer still does the deep sleep).
// LVGL task only.
void ClaraFaceDisplay::GoToSleep() {
    if (sleeping_) return;
    sleeping_ = true;
    sleep_at_us_ = esp_timer_get_time();
    sleep_idle_seen_ = false;
    last_hush_us_ = 0;
    held_ = false;
    laugh_until_us_ = look_until_us_ = boop_until_us_ = 0;
    ESP_LOGI(TAG, "held: going to sleep");
}

void ClaraFaceDisplay::WakeFromSleep(const char* why) {
    if (!sleeping_) return;
    sleeping_ = false;
    if (status_bar_) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    if (mouth_) lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
    if (subtitle_) lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    blink_step_ = 5;                                   // lids open
    ESP_LOGI(TAG, "awake: %s", why);
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
    int64_t waited = now - listening_since_us_;
    // sleepy waking: the lids part two thirds of the way in, and open fully at the end
    drowsy_ = waited >= kListenSettleMs * 1000 * 2 / 3;
    if (waited < kListenSettleMs * 1000) return false;
    ready_ = true;
    blink_step_ = 5;                           // the rest of a blink: lids 2, 1, 0
    ESP_LOGI(TAG, "Listening: eyes open");
    return true;
}

void ClaraFaceDisplay::Tick() {
    int64_t now = esp_timer_get_time();
    EarsBack(now);
    int mk = motion_kind_.exchange(-1);
    if (mk >= 0) OnMotion(mk, now);

    if (sleeping_) {
        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            sleep_idle_seen_ = true;
        } else if (sleep_idle_seen_) {                 // asleep, and the wake word opened a channel
            WakeFromSleep("conversation");
        } else if (now - last_hush_us_ > 500 * 1000) { // put to sleep mid-conversation: end it
            last_hush_us_ = now;
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "asleep: ending the conversation");
                app.ToggleChatState();                 // speaking: stops her; listening: closes the channel
            }
        }
        if (!sleeping_) {
            // woken just now
        } else {
            int lid = (int)((now - sleep_at_us_) / (120 * 1000));   // lids drift shut
            if (lid > kLidLevels - 1) lid = kLidLevels - 1;
            ApplyMouth(0);
            for (auto& eye : eyes_) { ApplyGaze(eye, 0, 0); ApplyLid(eye, lid); }
            if (lid == kLidLevels - 1) {                // eyes shut: nothing else on the screen
                if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
                if (mouth_) lv_obj_add_flag(mouth_, LV_OBJ_FLAG_HIDDEN);
                if (subtitle_) lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }
    }

    if (!ready_ && !BecameReady()) {
        ApplyMouth(0);
        for (auto& eye : eyes_) {
            ApplyGaze(eye, 0, 0);
            ApplyLid(eye, drowsy_ ? kLidLevels - 2 : kLidLevels - 1);   // a slit, or shut
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
    bool laughing = now < laugh_until_us_ || (kComposed && now < startle_until_us_);
    bool looking = now < look_until_us_;
    bool told = now < cmd_look_until_us_.load();       // the host said where to look
    float tx = looking ? look_x_ : told ? cmd_look_x_.load() : target_x_;
    float ty = looking ? look_y_ : told ? cmd_look_y_.load() : target_y_;
    looking = looking || told;
    float roll = pose_roll_.load();
    bool dizzy = !kComposed && now < startle_until_us_;
    if (dizzy) {                                        // shaken: the eyes swim
        tx = 0.4f * sinf(now / 1.0e6f * 9.0f);
        ty = 0.2f * cosf(now / 1.0e6f * 7.0f);
        looking = true;
    } else if (!looking && fabsf(roll) > 8.0f) {        // tilted: she looks toward the side that went up
        tx = -sinf(roll * (float)M_PI / 180.0f) * 0.45f;
        ty = target_y_;
        looking = true;
    }
    gaze_x_ += (tx - gaze_x_) * (looking ? 0.3f : 0.12f);
    gaze_y_ += (ty - gaze_y_) * (looking ? 0.3f : 0.12f);
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
    bool boop = now < boop_until_us_ || dizzy;
    int laugh_beat = (int)((now / 1000) / 350) % 2;     // a slow ha-ha
    int edy, emo;
    if (kComposed) {
        // Clara: a glance at the finger, patience when held, and when tickled a
        // half-lidded look with the eyes drifting up: "was there something you needed?"
        if (laughing) {
            lid = 2;
            tx = 0; ty = -0.3f;                          // eyes up
            gaze_x_ += (tx - gaze_x_) * 0.1f; gaze_y_ += (ty - gaze_y_) * 0.1f;
        } else if (held_) {
            lid = 1;                                    // a patient, heavy-lidded look
        }
        edy = laughing ? 4 : emotion_dy_.load();
        emo = laughing ? 3 : emotion_.load();           // the annoyed lid cut
    } else {
        if (laughing) {
            lid = laugh_beat ? 3 : 1;                   // eyes crinkle shut and open
            if (!speaking) ApplyMouth(laugh_beat ? 3 : 2);  // "ah" / "ee"
        } else if (held_) {
            lid = 3;                                    // squished: a contented squint
        } else if (boop) {
            lid = 0;                                    // wide
        }
        edy = laughing ? (laugh_beat ? -2 : -8) : boop ? -12 : held_ ? -4 : emotion_dy_.load();
        emo = (laughing || held_) ? 1 : emotion_.load();    // the happy lid cut
    }
    if (face_down_) lid = kLidLevels - 1;               // lying on her face: eyes shut
    if (edy != shown_emotion_dy_) {
        shown_emotion_dy_ = edy;
        PlaceEyes();
    }
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
void ClaraFaceDisplay::Motion(int kind) {
    motion_kind_.store(kind);
    // asleep on battery the face timer is paused and the panel is dark: being picked up
    // brings her back, as a tap would
    if (kind == ClaraMotion::kPickedUp && panel_asleep_ && on_touch_wake_) {
        Application::GetInstance().Schedule([this]() {
            if (panel_asleep_ && on_touch_wake_) { ESP_LOGI(TAG, "picked up while asleep on battery"); on_touch_wake_(); }
        });
    }
}

void ClaraFaceDisplay::Pose(float roll_deg, float up_z) {
    pose_roll_.store(roll_deg);
}

// LVGL task, from Tick. The clips: Clara is composed ("Gently, please." when shaken,
// "Hm?" when picked up); a playful character gets a "Whoa!" and wide eyes instead.
void ClaraFaceDisplay::OnMotion(int kind, int64_t now) {
    if (sleeping_) {
        if (kind == ClaraMotion::kPickedUp) WakeFromSleep("picked up");
        return;
    }
    if (!ready_) return;
    switch (kind) {
        case ClaraMotion::kPickedUp:
            if (kComposed) { if (blink_step_ < 0) blink_step_ = 0; }
            else boop_until_us_ = now + (int64_t)kBoopMs * 1000;
            look_x_ = 0; look_y_ = -0.3f;              // up, at whoever has her
            look_until_us_ = now + 1500 * 1000;
            Say(Lang::Sounds::OGG_HM, last_hm_us_);
            break;
        case ClaraMotion::kSetDown:
            if (blink_step_ < 0) blink_step_ = 0;
            break;
        case ClaraMotion::kShaken:
            startle_until_us_ = now + 2500 * 1000;
            if (kComposed) Say(Lang::Sounds::OGG_GENTLY, last_gently_us_);
            else Say(Lang::Sounds::OGG_WHOA, last_whoa_us_);
            break;
        case ClaraMotion::kFaceDown:
            face_down_ = true;
            break;
        default:                                       // upright, tilted, face up, upside down
            face_down_ = false;
            break;
    }
}

void ClaraFaceDisplay::SetLook(float x, float y, int hold_ms) {
    x = x < -1 ? -1 : x > 1 ? 1 : x;
    y = y < -1 ? -1 : y > 1 ? 1 : y;
    cmd_look_x_.store(x * 0.45f);                     // fractions of the eye radius, as the touch look
    cmd_look_y_.store(y * 0.30f);
    cmd_look_until_us_.store(esp_timer_get_time() + (int64_t)hold_ms * 1000);
    ESP_LOGI(TAG, "look %.1f,%.1f for %d ms", x, y, hold_ms);
}

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
