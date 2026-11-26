// Pantalla del tiempo con iconos animados para el ESP-SparkBot (datos de Open-Meteo).
#ifndef WEATHER_SCREEN_H
#define WEATHER_SCREEN_H

#include "screen_kit.h"

#include <lvgl.h>

#include <expected>
#include <string>

class Display;

class WeatherScreen {
public:
    explicit WeatherScreen(Display* display);
    void RegisterTools();

private:
    struct Place {
        std::string name;
        double latitude = 0;
        double longitude = 0;
        bool has_elevation = false;
        int elevation = 0;
    };

    struct Report {
        std::string place;
        std::string day_name;   // vacío = ahora mismo
        int code = 0;
        bool is_day = true;
        int temperature = 0;    // actual (o máxima si es otro día)
        int temp_min = 0;
        int temp_max = 0;
        int wind = 0;
        int rain_probability = 0;
    };

    Display* display_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* place_label_ = nullptr;
    lv_obj_t* desc_label_ = nullptr;
    lv_obj_t* minmax_label_ = nullptr;
    lv_obj_t* extra_label_ = nullptr;
    lv_obj_t* icon_box_ = nullptr;
    lv_obj_t* minus_ = nullptr;
    lv_obj_t* degree_ = nullptr;
    kit::Digit digits_[2];
    lv_timer_t* timer_ = nullptr;
    uint32_t visible_until_ = 0;

    std::expected<Place, std::string> Geocode(const std::string& name);
    std::expected<Report, std::string> Fetch(const Place& place, int days_ahead);
    static std::expected<std::string, std::string> HttpGet(const std::string& url);
    static std::string UrlEncode(const std::string& text);
    static const char* Describe(int code);
    static const char* DescribeSpoken(int code);

    void Show(const Report& report);
    void Build();
    void BuildIcon(int code, bool is_day);
    static void TimerCallback(lv_timer_t* timer);
};

#endif  // WEATHER_SCREEN_H
