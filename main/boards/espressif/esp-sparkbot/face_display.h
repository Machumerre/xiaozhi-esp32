// Cara propia para el ESP-SparkBot: ojos animados que parpadean, miran a los lados
// y cambian de expresión según la emoción que elige la IA.
#ifndef FACE_DISPLAY_H
#define FACE_DISPLAY_H

#include "display/lcd_display.h"

#include <lvgl.h>

#include <cstdint>
#include <string>

class SparkBotDisplay : public SpiLcdDisplay {
public:
    SparkBotDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                    bool swap_xy);

    virtual void SetupUI() override;
    virtual bool SetEmotion(const char* emotion) override;

private:
    enum class Shape { kNormal, kHappy, kSad, kAngry, kSurprised, kSleepy, kThinking, kWink };

    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {};
    lv_obj_t* lid_top_[2] = {};
    lv_obj_t* lid_bottom_[2] = {};
    lv_obj_t* mouth_[7] = {};
    lv_timer_t* timer_ = nullptr;

    Shape shape_ = Shape::kNormal;
    uint32_t emotion_color_ = 0x5FE3FF;
    uint32_t last_color_ = 0;
    int eye_w_ = 50;
    int eye_h_ = 66;

    // Animación
    uint32_t ticks_ = 0;
    uint32_t next_blink_ = 60;
    int blink_step_ = -1;
    uint32_t next_gaze_ = 40;
    float gaze_x_ = 0, gaze_y_ = 0;
    float target_x_ = 0, target_y_ = 0;

    void BuildFace();
    void ApplyShape();
    static void TimerCallback(lv_timer_t* timer);
    void OnTimer();
};

#endif  // FACE_DISPLAY_H
