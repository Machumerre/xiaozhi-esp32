// Navegador para el coche: el móvil (MacroDroid) reenvía al robot las indicaciones de Google Maps
// por wifi y el robot las dibuja en estilo HUD.
#ifndef NAV_SCREEN_H
#define NAV_SCREEN_H

#include <esp_http_server.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <mutex>
#include <string>

class Display;

class NavScreen {
public:
    explicit NavScreen(Display* display);
    void RegisterTools();

private:
    enum class Turn { kStraight, kLeft, kRight, kSlightLeft, kSlightRight, kUTurn, kRoundabout, kArrive };

    struct Step {
        Turn turn = Turn::kStraight;
        int roundabout_exit = 0;
        std::string distance;  // "250" / "1,2"
        std::string unit;      // "m" / "km"
        std::string instruction;
        std::string street;
        std::string eta;       // "18:42"
        std::string remaining; // "12 min · 5,3 km"
    };

    Display* display_;
    std::mutex mutex_;
    httpd_handle_t server_ = nullptr;
    esp_timer_handle_t start_timer_ = nullptr;
    int updates_ = 0;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* eta_label_ = nullptr;
    lv_obj_t* arrow_label_ = nullptr;
    lv_obj_t* ring_ = nullptr;
    lv_obj_t* ring_exit_ = nullptr;
    lv_obj_t* distance_label_ = nullptr;
    lv_obj_t* unit_label_ = nullptr;
    lv_obj_t* instruction_label_ = nullptr;
    lv_obj_t* street_label_ = nullptr;
    lv_obj_t* remaining_label_ = nullptr;
    lv_timer_t* hide_timer_ = nullptr;
    uint32_t visible_until_ = 0;

    static void StartTimerCallback(void* arg);
    void TryStartServer();
    static esp_err_t HandleNav(httpd_req_t* req);
    static esp_err_t HandleEnd(httpd_req_t* req);
    static std::string QueryValue(const std::string& query, const char* key);
    static Step Parse(const std::string& title, const std::string& text, const std::string& sub);
    static std::string IpAddress();

    void Build();
    void Show(const Step& step);
    void Hide();
    static void HideCallback(lv_timer_t* timer);
};

#endif  // NAV_SCREEN_H
