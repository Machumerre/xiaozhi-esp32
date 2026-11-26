// Agenda del SparkBot: eventos con recordatorio, vista de mes y próximos días (incluye exámenes).
#ifndef AGENDA_SCREEN_H
#define AGENDA_SCREEN_H

#include <esp_timer.h>
#include <lvgl.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class Display;

class AgendaScreen {
public:
    explicit AgendaScreen(Display* display);
    void RegisterTools();

private:
    struct Event {
        int year = 0, month = 0, day = 0;
        int hour = -1, minute = 0;  // hour = -1: todo el día
        std::string title;
        bool exam = false;
        int64_t Days() const;
    };

    Display* display_;
    std::mutex mutex_;
    std::vector<Event> events_;
    std::set<std::string> reminded_;
    esp_timer_handle_t timer_ = nullptr;

    // Pantalla
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* right_label_ = nullptr;
    lv_obj_t* month_view_ = nullptr;
    lv_obj_t* list_view_ = nullptr;
    lv_obj_t* cell_bg_[42] = {};
    lv_obj_t* cell_label_[42] = {};
    lv_obj_t* cell_dot_[42] = {};
    lv_obj_t* row_date_[6] = {};
    lv_obj_t* row_time_[6] = {};
    lv_obj_t* row_title_[6] = {};
    lv_obj_t* empty_label_ = nullptr;
    lv_timer_t* hide_timer_ = nullptr;
    uint32_t visible_until_ = 0;

    void Load();
    void Save();
    std::vector<Event> WithExams();
    static std::string EventText(const Event& e, bool with_date);

    void Tick();
    static void TickCallback(void* arg);

    void Build();
    void ShowMonth(int year, int month, const std::vector<Event>& events);
    void ShowUpcoming(const std::vector<Event>& events, int64_t today, int days);
    void Reveal(bool month);
    static void HideCallback(lv_timer_t* timer);
};

#endif  // AGENDA_SCREEN_H
