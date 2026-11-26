// Alarmas con amanecer animado y resumen de buenos días para el ESP-SparkBot.
#ifndef ALARM_SCREEN_H
#define ALARM_SCREEN_H

#include <esp_timer.h>
#include <lvgl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Display;

class AlarmScreen {
public:
    explicit AlarmScreen(Display* display);
    void RegisterTools();

    bool IsRinging();
    // Para la alarma; si briefing es true, pide a la IA el resumen del día
    void Dismiss(bool briefing);

private:
    struct Alarm {
        int hour = 0;
        int minute = 0;
        uint8_t days = 0;  // bit 0 = lunes ... bit 6 = domingo; 0 = una sola vez
        bool briefing = true;
        std::string label;
    };

    Display* display_;
    std::mutex mutex_;
    esp_timer_handle_t timer_ = nullptr;
    std::vector<Alarm> alarms_;
    int last_checked_minute_ = -1;

    // Sonando
    bool ringing_ = false;
    bool ring_briefing_ = false;
    int ring_seconds_ = 0;
    std::string ring_label_;

    // Pantalla
    lv_obj_t* root_ = nullptr;
    lv_obj_t* sky_ = nullptr;
    lv_obj_t* sun_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;

    void Load();
    void Save();
    static bool ParseTime(const std::string& text, int& hour, int& minute);
    static bool ParseDays(const std::string& text, uint8_t& days);
    static std::string DaysText(uint8_t days);
    std::string ListText();

    void Tick();
    static void TickCallback(void* arg);
    void StartRinging(const Alarm& alarm);
    void StopRinging(bool briefing);

    void Build();
    void ShowScreen(bool show);
    void UpdateTime();
    static void SetSunrise(void* obj, int32_t progress);
};

#endif  // ALARM_SCREEN_H
