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
    virtual void SetChatMessage(const char* role, const char* content) override;

    // Herramientas propias de la pantalla (modo noche)
    void RegisterTools();

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

    // Pantalla de "pensando": reactor con anillos que giran
    static constexpr int kTicks = 48;
    lv_obj_t* think_ = nullptr;
    lv_obj_t* think_arcs_[4] = {};
    lv_obj_t* think_core_ = nullptr;
    lv_obj_t* think_time_ = nullptr;
    lv_obj_t* think_label_ = nullptr;
    lv_obj_t* think_ticks_[kTicks] = {};
    uint8_t think_tick_level_[kTicks] = {};
    lv_obj_t* think_orbit_[3] = {};
    bool thinking_ = false;
    bool think_visible_ = false;
    uint32_t think_start_ms_ = 0;

    // Iconos tipo smartwatch (wifi y batería)
    lv_obj_t* wifi_bars_[4] = {};
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_text_ = nullptr;
    int last_wifi_level_ = -1;
    int last_battery_ = -2;

    // Pie de reposo: nivel, racha y minutos de hoy
    lv_obj_t* footer_ = nullptr;
    lv_obj_t* footer_label_ = nullptr;
    lv_obj_t* xp_bar_ = nullptr;

    // Subida de nivel
    lv_obj_t* levelup_ = nullptr;
    lv_obj_t* levelup_ring_ = nullptr;
    lv_obj_t* levelup_number_ = nullptr;
    int levelup_ticks_ = 0;

    // Arranque tipo "sistema iniciándose"
    static constexpr int kBootLines = 5;
    lv_obj_t* boot_ = nullptr;
    lv_obj_t* boot_lines_[kBootLines] = {};
    lv_obj_t* boot_status_[kBootLines] = {};
    lv_obj_t* boot_bar_ = nullptr;
    int boot_ticks_ = 0;
    int boot_fade_ = -1;

    // Modo noche: pantalla tenue, ojos dormidos y "zzz"; bosteza al despertar
    bool night_enabled_ = true;
    int night_start_ = 23 * 60 + 30;  // minutos desde medianoche
    int night_end_ = 7 * 60 + 30;
    bool night_window_ = false;
    bool night_active_ = false;
    uint32_t last_busy_tick_ = 0;
    int yawn_ticks_ = 0;
    lv_obj_t* zzz_[3] = {};

    // Reloj de reposo
    lv_obj_t* clock_ = nullptr;
    lv_obj_t* clock_time_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    bool clock_visible_ = false;
    int last_clock_second_ = -1;

    void BuildFace();
    void BuildClock();
    void BuildStatusIcons();
    void BuildFooter();
    void UpdateFooter();
    void ShowLevelUp(int level);
    void AnimateLevelUp();
    void BuildBoot();
    void BuildNight();
    void UpdateNight(int state);
    void SetNight(bool on, bool woke_by_voice);
    void AnimateYawn();
    bool AnimateBoot(int state);
    void UpdateStatusIcons();
    void StyleSubtitles();
    void UpdateClock(bool idle);
    void BuildThinking();
    void ShowThinking(bool show);
    void AnimateThinking();
    void ApplyShape();
    static void TimerCallback(lv_timer_t* timer);
    void OnTimer();
};

#endif  // FACE_DISPLAY_H
