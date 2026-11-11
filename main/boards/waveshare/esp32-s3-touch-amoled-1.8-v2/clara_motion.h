#pragma once

#include <driver/i2c_master.h>
#include <functional>
#include <cstdint>

// The QMI8658 IMU on the board's I2C bus, read on its own task at 50 Hz. Gravity is
// low-passed out of the accelerometer; what is left is motion. From the two it decides
// "picked up", "set down", "shaken" and the orientation, and tells the board (which tells
// the face, and the host). The decisions live here so the LVGL task never waits on I2C.
class ClaraMotion {
public:
    enum Kind { kPickedUp = 0, kSetDown, kShaken, kTilted, kUpright, kFaceDown, kFaceUp, kUpsideDown };
    struct Pose {
        float roll_deg;            // + when the top of the panel leans to the viewer's right
        float pitch_deg;           // + when the top leans back (the screen turns toward the ceiling)
        float up_x, up_y, up_z;    // the measured "up" in face axes: right, up, out of the screen
    };
    // called from the IMU task: record, never draw
    using EventFn = std::function<void(Kind, const Pose&)>;
    using PoseFn = std::function<void(const Pose&)>;    // ten times a second, for the eyes

    bool Start(i2c_master_bus_handle_t bus, EventFn on_event, PoseFn on_pose);
    static const char* Name(Kind k);                     // "picked_up", a token for the host
    static const char* Phrase(Kind k, const Pose& p);    // "picked up", "tilted to the left": for people

private:
    static void Task(void* arg);
    void Run();
    bool ReadAccel(float& x, float& y, float& z);        // in g, face axes
    i2c_master_dev_handle_t dev_ = nullptr;
    EventFn on_event_;
    PoseFn on_pose_;
};
