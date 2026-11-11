#include "clara_motion.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

#define TAG "ClaraMotion"

// QMI8658: WHO_AM_I 0x00 = 0x05; CTRL1 0x02 (address auto-increment); CTRL2 0x03
// accelerometer range and rate; CTRL7 0x08 bit 0 enables the accelerometer; AX_L at 0x35.
static const uint8_t kAddrs[] = {0x6B, 0x6A};
static const float kLsbPerG = 4096.0f;                // at +/-8 g

// Chip axes to face axes (right, up, out of the screen), from the tilt test of 2026-11-11:
// standing upright the chip's +x points up, lying screen-up its -z does, and a top edge
// tilted to the viewer's left puts gravity's "up" in +y. The log prints the pose if a
// different board needs its own mapping.
static inline void MapAxes(float cx, float cy, float cz, float& rx, float& ry, float& rz) {
    rx = cy;
    ry = cx;
    rz = -cz;
}

// thresholds, in g and samples at 25 Hz
static const float kMotionG = 0.18f;                  // a sample that counts as moving
static const float kQuietG = 0.10f;                   // a sample that counts as still
static const float kBigG = 0.90f;                     // a sample that counts toward a shake (a brisk pick-up stays under)
static const int kPickUpSamples = 3;                  // moving samples in the last half second
static const int kSetDownSamples = 38;                // still for 1.5 s
static const int kShakeSamples = 5;                   // big samples in the last second
static const int kOrientHold = 15;                    // a new orientation must hold 0.6 s
static const float kTiltDeg = 35.0f;
static const int64_t kSettleUs = 3 * 1000 * 1000;    // gravity needs a moment after power-on

const char* ClaraMotion::Name(Kind k) {
    switch (k) {
        case kPickedUp: return "picked_up";
        case kSetDown: return "set_down";
        case kShaken: return "shaken";
        case kTilted: return "tilted";
        case kUpright: return "upright";
        case kFaceDown: return "face_down";
        case kFaceUp: return "face_up";
        case kUpsideDown: return "upside_down";
    }
    return "?";
}

const char* ClaraMotion::Phrase(Kind k, const Pose& p) {
    switch (k) {
        case kPickedUp: return "picked up";
        case kSetDown: return "set down";
        case kShaken: return "shaken";
        case kTilted: return p.roll_deg > 0 ? "tilted to the right" : "tilted to the left";
        case kUpright: return "stood upright again";
        case kFaceDown: return "laid face down";
        case kFaceUp: return "laid flat, face up";
        case kUpsideDown: return "turned upside down";
    }
    return "?";
}

bool ClaraMotion::Start(i2c_master_bus_handle_t bus, EventFn on_event, PoseFn on_pose) {
    on_event_ = std::move(on_event);
    on_pose_ = std::move(on_pose);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (uint8_t addr : kAddrs) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
        i2c_device_config_t cfg = {};
        cfg.device_address = addr;
        cfg.scl_speed_hz = 400000;
        err = i2c_master_bus_add_device(bus, &cfg, &dev_);
        if (err == ESP_OK) ESP_LOGI(TAG, "QMI8658 at 0x%02x", addr);
        break;
    }
    if (err != ESP_OK || dev_ == nullptr) {
        ESP_LOGW(TAG, "no IMU answered (%s)", esp_err_to_name(err));
        return false;
    }
    auto write = [this](uint8_t reg, uint8_t val) {
        uint8_t data[2] = {reg, val};
        return i2c_master_transmit(dev_, data, 2, 50);
    };
    uint8_t reg = 0x00, id = 0;
    if (i2c_master_transmit_receive(dev_, &reg, 1, &id, 1, 50) != ESP_OK || id != 0x05) {
        ESP_LOGW(TAG, "not a QMI8658 (id 0x%02x)", id);
        return false;
    }
    if (write(0x02, 0x60) != ESP_OK || write(0x03, 0x25) != ESP_OK || write(0x08, 0x01) != ESP_OK) {
        ESP_LOGW(TAG, "IMU setup failed");
        return false;
    }
    xTaskCreate(Task, "clara_motion", 4096, this, 3, nullptr);
    return true;
}

bool ClaraMotion::ReadAccel(float& x, float& y, float& z) {
    uint8_t reg = 0x35, d[6] = {};
    if (i2c_master_transmit_receive(dev_, &reg, 1, d, 6, 20) != ESP_OK) return false;
    auto dec = [](const uint8_t* p) { return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); };
    MapAxes(dec(d) / kLsbPerG, dec(d + 2) / kLsbPerG, dec(d + 4) / kLsbPerG, x, y, z);
    return true;
}

void ClaraMotion::Task(void* arg) {
    static_cast<ClaraMotion*>(arg)->Run();
    vTaskDelete(nullptr);
}

void ClaraMotion::Run() {
    float gx = 0, gy = 0, gz = 0;
    bool have_g = false;
    bool moving[12] = {}, big[25] = {};
    int mi = 0, bi = 0, still_n = 0, errors = 0, tick = 0;
    bool held = false;
    int64_t last_shake_us = 0, last_pose_log_us = 0;
    Kind orient = kUpright, cand = kUpright;
    int cand_n = 0;
    const int64_t start_us = esp_timer_get_time();

    auto fire = [this](Kind k, const Pose& p) {
        ESP_LOGI(TAG, "%s (roll %.0f, pitch %.0f)", Phrase(k, p), p.roll_deg, p.pitch_deg);
        if (on_event_) on_event_(k, p);
    };

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(40));                // 25 Hz: the bus is shared with the power chip, codec and touch
        float ax, ay, az;
        if (!ReadAccel(ax, ay, az)) {
            if (++errors == 50) ESP_LOGW(TAG, "IMU stopped answering");
            continue;
        }
        errors = 0;
        if (!have_g) { gx = ax; gy = ay; gz = az; have_g = true; }
        else { gx += (ax - gx) * 0.15f; gy += (ay - gy) * 0.15f; gz += (az - gz) * 0.15f; }
        float dx = ax - gx, dy = ay - gy, dz = az - gz;
        float motion = sqrtf(dx * dx + dy * dy + dz * dz);

        moving[mi++ % 12] = motion > kMotionG;
        big[bi++ % 25] = motion > kBigG;
        still_n = motion < kQuietG ? still_n + 1 : 0;
        int moving_n = 0, big_n = 0;
        for (bool m : moving) moving_n += m;
        for (bool b : big) big_n += b;

        float n = sqrtf(gx * gx + gy * gy + gz * gz);
        if (n < 0.3f) continue;                       // free fall or a bad read
        Pose p;
        p.up_x = gx / n; p.up_y = gy / n; p.up_z = gz / n;
        p.roll_deg = atan2f(-p.up_x, p.up_y) * 180.0f / (float)M_PI;   // + : the top leans to the viewer's right
        p.pitch_deg = atan2f(p.up_z, p.up_y) * 180.0f / (float)M_PI;
        int64_t now = esp_timer_get_time();
        if (tick++ % 3 == 0 && on_pose_) on_pose_(p);
        if (now - last_pose_log_us > 10 * 1000 * 1000) {   // a line every 10 s: for the axis test
            last_pose_log_us = now;
            ESP_LOGI(TAG, "pose up=(%.2f, %.2f, %.2f) roll %.0f pitch %.0f motion %.2f",
                     p.up_x, p.up_y, p.up_z, p.roll_deg, p.pitch_deg, motion);
        }
        if (now - start_us < kSettleUs) continue;

        if (big_n >= kShakeSamples && now - last_shake_us > 3 * 1000 * 1000) {
            last_shake_us = now;
            memset(big, 0, sizeof(big));
            fire(kShaken, p);
        }
        if (!held && moving_n >= kPickUpSamples) {
            held = true;
            fire(kPickedUp, p);
        } else if (held && still_n >= kSetDownSamples) {
            held = false;
            fire(kSetDown, p);
        }

        // orientation: a change counts once it has held for a moment; in-between poses keep the last
        Kind c = orient;
        if (p.up_z > 0.75f) c = kFaceUp;
        else if (p.up_z < -0.75f) c = kFaceDown;
        else if (p.up_y < -0.6f) c = kUpsideDown;
        else if (fabsf(p.roll_deg) > kTiltDeg && p.up_y > 0.3f) c = kTilted;   // roll means little lying flat
        else if (p.up_y > 0.8f) c = kUpright;
        if (c != cand) { cand = c; cand_n = 0; }
        else cand_n++;
        if (cand != orient && cand_n >= kOrientHold) {
            orient = cand;
            fire(orient, p);
        }
    }
}
