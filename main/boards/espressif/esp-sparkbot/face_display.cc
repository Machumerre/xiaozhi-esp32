// Cara propia para el ESP-SparkBot: ojos animados.
#include "face_display.h"

#include "application.h"
#include "board.h"
#include "companion.h"
#include "assets/lang_config.h"
#include "display/lvgl_display/lvgl_font.h"
#include "screen_kit.h"

#include <esp_random.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr uint32_t kCyan = 0x5FE3FF;
constexpr uint32_t kListen = 0x4DFFA0;
constexpr uint32_t kBusy = 0xFFB300;
constexpr int kEyeGap = 44;     // distancia del centro a cada ojo
constexpr int kEyeY = -10;      // ojos un poco por encima del centro (deja sitio al reloj)
constexpr int kMouthY = 44;     // boca debajo de los ojos

// Reactor de "pensando"
constexpr int kCX = 120;
constexpr int kCY = 114;
constexpr int kTickRadius = 98;
constexpr int kOrbitRadius = 59;
constexpr uint32_t kViolet = 0xB388FF;
constexpr uint32_t kTickColors[4] = {0x173540, 0x2A7F99, 0x5FE3FF, 0xFFFFFF};

uint32_t RandomRange(uint32_t min, uint32_t max) { return min + esp_random() % (max - min + 1); }

lv_obj_t* CreatePlain(lv_obj_t* parent, uint32_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

bool Is(const std::string& value, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (value == name) {
            return true;
        }
    }
    return false;
}

lv_obj_t* MakeArc(lv_obj_t* parent, int r, int w, uint32_t color, int start, int end) {
    lv_obj_t* arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, 2 * r, 2 * r);
    lv_obj_set_pos(arc, kCX - r, kCY - r);
    lv_arc_set_bg_angles(arc, start, end);
    lv_obj_set_style_arc_width(arc, w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    return arc;
}

void SetArcRotation(void* obj, int32_t v) { lv_arc_set_rotation(static_cast<lv_obj_t*>(obj), v); }
void SetShadowOpa(void* obj, int32_t v) {
    lv_obj_set_style_shadow_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

}  // namespace

SparkBotDisplay::SparkBotDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void SparkBotDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    // Textos de estado ("Escuchando...") con la fuente de consola
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_font(status_label_, &font_mono_14, 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_font(notification_label_, &font_mono_14, 0);
    }
    // Los iconos originales se ocultan: dibujamos los nuestros tipo smartwatch
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_opa(network_label_, LV_OPA_TRANSP, 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_opa(battery_label_, LV_OPA_TRANSP, 0);
    }
    StyleSubtitles();
    BuildFace();
}

// Subtítulos con la fuente de consola. Ojo: sin fuente de respaldo, porque la fuente del tema
// se sustituye al cargar los recursos y un puntero a la antigua hacía que el robot se colgara.
// Lo que no tenga la fuente (emojis, otros alfabetos) simplemente no se dibuja.
void SparkBotDisplay::StyleSubtitles() {
    if (chat_message_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(chat_message_label_, &font_mono_18, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xCFF6FF), 0);
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_side(bottom_bar_, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(bottom_bar_, 1, 0);
        lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(kit::kHudLine), 0);
    }
}

void SparkBotDisplay::BuildFace() {
    if (face_ != nullptr) {
        return;
    }
    lv_obj_t* screen = lv_screen_active();
    face_ = lv_obj_create(screen);
    lv_obj_remove_style_all(face_);
    lv_obj_set_size(face_, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(face_, LV_OBJ_FLAG_CLICKABLE);
    // Justo encima del fondo y por debajo de las barras de estado y de los subtítulos
    lv_obj_move_to_index(face_, 1);

    for (int i = 0; i < 2; ++i) {
        eye_[i] = CreatePlain(face_, kCyan);
        lv_obj_align(eye_[i], LV_ALIGN_CENTER, i == 0 ? -kEyeGap : kEyeGap, kEyeY);
        lid_top_[i] = CreatePlain(eye_[i], 0x000000);
        lid_bottom_[i] = CreatePlain(eye_[i], 0x000000);
        lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 7; ++i) {
        mouth_[i] = CreatePlain(face_, kCyan);
        lv_obj_set_size(mouth_[i], 6, 8);
        lv_obj_set_style_radius(mouth_[i], 3, 0);
        lv_obj_align(mouth_[i], LV_ALIGN_CENTER, (i - 3) * 11, kMouthY);
        lv_obj_add_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    ApplyShape();
    BuildStatusIcons();
    BuildClock();
    BuildFooter();
    BuildBoot();
    BuildThinking();
    timer_ = lv_timer_create(&SparkBotDisplay::TimerCallback, 40, this);
}

void SparkBotDisplay::BuildStatusIcons() {
    // Wifi: cuatro barras de cobertura
    for (int i = 0; i < 4; ++i) {
        const int h = 4 + i * 3;
        wifi_bars_[i] = kit::Box(face_, 14 + i * 5, 22 - h, 3, h, 1, kit::kHudLine);
    }
    // Batería: pila con relleno y porcentaje debajo
    battery_ = kit::Group(face_, 200, 8, 28, 32);
    lv_obj_t* body = kit::Box(battery_, 0, 0, 24, 12, 3, 0x000000);
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(kit::kHudDim), 0);
    kit::Box(battery_, 24, 4, 2, 4, 1, kit::kHudDim);
    battery_fill_ = kit::Box(battery_, 2, 2, 20, 8, 1, kit::kHudCyan);
    battery_text_ = kit::Text(battery_, -4, 14, 32, &font_mono_14, kit::kHudDim);
    lv_obj_add_flag(battery_, LV_OBJ_FLAG_HIDDEN);
}

// Cada 2 segundos: cobertura wifi y batería
void SparkBotDisplay::UpdateStatusIcons() {
    int wifi_level = 0;
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        wifi_level = ap.rssi >= -55 ? 4 : (ap.rssi >= -65 ? 3 : (ap.rssi >= -75 ? 2 : 1));
    }
    if (wifi_level != last_wifi_level_) {
        last_wifi_level_ = wifi_level;
        for (int i = 0; i < 4; ++i) {
            uint32_t color = i < wifi_level ? kit::kHudCyan : kit::kHudLine;
            if (wifi_level == 0) {
                color = 0x6B2A2A;  // sin conexión
            }
            lv_obj_set_style_bg_color(wifi_bars_[i], lv_color_hex(color), 0);
        }
    }

    int level = -1;
    bool charging = false, discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        level = -1;
    }
    const int key = level < 0 ? -1 : level * 2 + (charging ? 1 : 0);
    if (key == last_battery_) {
        return;
    }
    last_battery_ = key;
    if (level < 0) {
        lv_obj_add_flag(battery_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    level = std::max(0, std::min(100, level));
    lv_obj_remove_flag(battery_, LV_OBJ_FLAG_HIDDEN);
    const uint32_t color = charging ? kit::kHudCyan : (level <= 15 ? 0xFF4A4A : (level <= 35 ? 0xFFB300 : 0x4DFFA0));
    lv_obj_set_width(battery_fill_, std::max(2, 20 * level / 100));
    lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(color), 0);
    char text[24];
    snprintf(text, sizeof(text), "%d%%", level);
    lv_label_set_text(battery_text_, text);
}

// ---------------------------------------------------------------------------
// Pie de reposo y subida de nivel
// ---------------------------------------------------------------------------

void SparkBotDisplay::BuildFooter() {
    footer_ = kit::Group(face_, 0, 168, 240, 36);
    lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
    footer_label_ = kit::Centered(footer_, 0, &font_mono_14, kit::kHudDim, 224);
    kit::Box(footer_, 70, 22, 100, 3, 1, kit::kHudLine);
    xp_bar_ = kit::Box(footer_, 70, 22, 2, 3, 1, kit::kHudCyan);
}

void SparkBotDisplay::UpdateFooter() {
    auto& companion = Companion::Get();
    const int new_level = companion.TakeLevelUp();
    if (new_level > 0) {
        ShowLevelUp(new_level);
    }
    const auto s = companion.GetSnapshot();
    char text[64];
    if (s.streak > 0) {
        snprintf(text, sizeof(text), "NV %d \u00B7 RACHA %d \u00B7 HOY %d'", s.level, s.streak, s.today_minutes);
    } else {
        snprintf(text, sizeof(text), "NV %d \u00B7 HOY %d'", s.level, s.today_minutes);
    }
    lv_label_set_text(footer_label_, text);
    kit::SetColor(footer_label_, s.streak > 0 && !s.studied_today ? 0xFFB300 : kit::kHudDim);
    const int span = std::max(1, s.next_level_xp - s.level_start_xp);
    const int filled = std::max(2, std::min(100, (s.xp - s.level_start_xp) * 100 / span));
    lv_obj_set_width(xp_bar_, filled);
}

void SparkBotDisplay::ShowLevelUp(int level) {
    if (levelup_ == nullptr) {
        levelup_ = kit::Overlay();
        levelup_ring_ = kit::Box(levelup_, 120, 112, 0, 0, LV_RADIUS_CIRCLE, 0x000000);
        lv_obj_set_style_bg_opa(levelup_ring_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(levelup_ring_, 3, 0);
        lv_obj_set_style_border_color(levelup_ring_, lv_color_hex(kCyan), 0);
        lv_obj_t* title = kit::Centered(levelup_, 30, &font_mono_20, kit::kHudCyan);
        lv_obj_set_style_text_letter_space(title, 4, 0);
        lv_label_set_text(title, "\u25B8 NIVEL");
        levelup_number_ = kit::Centered(levelup_, 64, &font_mono_80, 0xFFFFFF);
        lv_obj_t* sub = kit::Centered(levelup_, 170, &font_mono_14, kit::kHudDim);
        lv_label_set_text(sub, "NOS CONOCEMOS MEJOR");
        kit::Corners(levelup_);
    }
    char text[24];
    snprintf(text, sizeof(text), "%d", level);
    lv_label_set_text(levelup_number_, text);
    levelup_ticks_ = 125;  // 5 segundos
    lv_obj_remove_flag(levelup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(levelup_);
    Application::GetInstance().Schedule(
        []() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS); });
}

void SparkBotDisplay::AnimateLevelUp() {
    levelup_ticks_--;
    // Onda que se expande desde el centro una y otra vez
    const int phase = (125 - levelup_ticks_) % 30;
    const int size = 40 + phase * 7;
    lv_obj_set_size(levelup_ring_, size, size);
    lv_obj_set_pos(levelup_ring_, 120 - size / 2, 112 - size / 2);
    lv_obj_set_style_border_opa(levelup_ring_, static_cast<lv_opa_t>(255 - phase * 8), 0);
    const bool flash = (levelup_ticks_ / 6) % 2 == 0;
    kit::SetColor(levelup_number_, flash ? 0xFFFFFF : kCyan);
    if (levelup_ticks_ <= 0) {
        lv_obj_add_flag(levelup_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Arranque: líneas de "sistema iniciándose" hasta que el robot está listo
// ---------------------------------------------------------------------------

void SparkBotDisplay::BuildBoot() {
    boot_ = kit::Overlay();
    lv_obj_remove_flag(boot_, LV_OBJ_FLAG_HIDDEN);
    kit::Corners(boot_);
    lv_obj_t* logo = kit::Centered(boot_, 30, &font_mono_20, kCyan);
    lv_obj_set_style_text_letter_space(logo, 3, 0);
    lv_label_set_text(logo, "J.A.R.V.I.S.");
    lv_obj_t* sub = kit::Centered(boot_, 56, &font_mono_14, kit::kHudDim);
    lv_label_set_text(sub, "INICIANDO SISTEMAS");
    static const char* kNames[kBootLines] = {"> N\u00DACLEO", "> AUDIO", "> PANTALLA", "> RED", "> SERVIDOR IA"};
    for (int i = 0; i < kBootLines; ++i) {
        boot_lines_[i] = kit::Text(boot_, 24, 88 + i * 20, 130, &font_mono_14, 0xB0BEC5, LV_TEXT_ALIGN_LEFT);
        lv_label_set_text(boot_lines_[i], kNames[i]);
        boot_status_[i] = kit::Text(boot_, 150, 88 + i * 20, 66, &font_mono_14, 0x4DFFA0, LV_TEXT_ALIGN_RIGHT);
        lv_obj_add_flag(boot_lines_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(boot_status_[i], LV_OBJ_FLAG_HIDDEN);
    }
    kit::Box(boot_, 30, 200, 180, 4, 2, kit::kHudLine);
    boot_bar_ = kit::Box(boot_, 30, 200, 4, 4, 2, kCyan);
}

// Devuelve true mientras la pantalla de arranque tapa todo
bool SparkBotDisplay::AnimateBoot(int state_value) {
    const auto state = static_cast<DeviceState>(state_value);
    boot_ticks_++;
    // Si hay que configurar el wifi o activar el robot, se quita para que veas las instrucciones
    const bool must_hide = state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating ||
                           state == kDeviceStateFatalError || state == kDeviceStateUpgrading || boot_ticks_ > 500;
    const bool network_up = state != kDeviceStateUnknown && state != kDeviceStateStarting;
    const bool ready = state == kDeviceStateIdle || state == kDeviceStateListening ||
                       state == kDeviceStateConnecting || state == kDeviceStateSpeaking;

    if (boot_fade_ < 0 && !must_hide) {
        for (int i = 0; i < kBootLines; ++i) {
            if (boot_ticks_ < 8 * (i + 1)) break;
            lv_obj_remove_flag(boot_lines_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(boot_status_[i], LV_OBJ_FLAG_HIDDEN);
            bool ok = i < 3 || (i == 3 && network_up) || (i == 4 && ready);
            if (ok) {
                lv_label_set_text(boot_status_[i], "[ OK ]");
                kit::SetColor(boot_status_[i], 0x4DFFA0);
            } else {
                static const char* kSpin[4] = {"[    ]", "[.   ]", "[..  ]", "[... ]"};
                lv_label_set_text(boot_status_[i], kSpin[(boot_ticks_ / 4) % 4]);
                kit::SetColor(boot_status_[i], 0xFFB300);
            }
        }
        int done = 0;
        for (int i = 0; i < kBootLines; ++i) {
            if (boot_ticks_ >= 8 * (i + 1) && (i < 3 || (i == 3 && network_up) || (i == 4 && ready))) done++;
        }
        lv_obj_set_width(boot_bar_, std::max(4, 180 * done / kBootLines));
        if (done == kBootLines && boot_ticks_ >= 60) {
            boot_fade_ = 10;
        }
    }
    if (must_hide && boot_fade_ < 0) {
        boot_fade_ = 1;
    }
    if (boot_fade_ >= 0) {
        lv_obj_set_style_opa(boot_, static_cast<lv_opa_t>(255 * boot_fade_ / 10), 0);
        if (--boot_fade_ < 0) {
            lv_obj_delete(boot_);
            boot_ = nullptr;
            return false;
        }
    }
    return true;
}

void SparkBotDisplay::BuildClock() {
    clock_ = kit::Group(face_, 0, 0, 240, 70);
    lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);
    clock_time_ = kit::Centered(clock_, 4, &font_mono_36, 0xE6F7FF, 200);
    lv_obj_set_style_text_letter_space(clock_time_, 1, 0);
    clock_date_ = kit::Centered(clock_, 44, &font_mono_14, kit::kHudDim, 120);
    lv_obj_set_style_text_letter_space(clock_date_, 2, 0);
    kit::HLine(clock_, 32, 53, 30);
    kit::HLine(clock_, 178, 53, 30);
    kit::Box(clock_, 28, 51, 5, 5, LV_RADIUS_CIRCLE, kit::kHudLine);
    kit::Box(clock_, 207, 51, 5, 5, LV_RADIUS_CIRCLE, kit::kHudLine);
}

// Reloj grande cuando está en reposo; el texto de estado de arriba se oculta mientras tanto
void SparkBotDisplay::UpdateClock(bool idle) {
    if (idle != clock_visible_) {
        clock_visible_ = idle;
        last_clock_second_ = -1;
        if (idle) {
            lv_obj_remove_flag(clock_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(clock_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(footer_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label_ != nullptr) {
            lv_obj_set_style_text_opa(status_label_, idle ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        }
    }
    if (!idle) {
        return;
    }
    const time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_sec == last_clock_second_) {
        return;
    }
    last_clock_second_ = local.tm_sec;
    if (local.tm_year < 2025 - 1900) {
        lv_label_set_text(clock_time_, "--:--");
        lv_label_set_text(clock_date_, "");
        return;
    }
    static const char* kDays[7] = {"DOM", "LUN", "MAR", "MI\u00C9", "JUE", "VIE", "S\u00C1B"};
    static const char* kMonths[12] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN",
                                      "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};
    char text[24];
    // Los dos puntos parpadean cada segundo
    snprintf(text, sizeof(text), (local.tm_sec % 2 == 0) ? "%02d:%02d" : "%02d %02d", local.tm_hour,
             local.tm_min);
    lv_label_set_text(clock_time_, text);
    snprintf(text, sizeof(text), "%s %d %s", kDays[local.tm_wday], local.tm_mday, kMonths[local.tm_mon]);
    lv_label_set_text(clock_date_, text);
}

void SparkBotDisplay::BuildThinking() {
    think_ = kit::Group(face_, 0, 0, 240, 240);
    lv_obj_add_flag(think_, LV_OBJ_FLAG_HIDDEN);

    // Anillo de marcas como un dial; un destello lo recorre
    for (int i = 0; i < kTicks; ++i) {
        const double angle = i * 2.0 * M_PI / kTicks - M_PI / 2;
        const int size = (i % 4 == 0) ? 5 : 3;
        const int x = kCX + static_cast<int>(std::lround(kTickRadius * std::cos(angle))) - size / 2;
        const int y = kCY + static_cast<int>(std::lround(kTickRadius * std::sin(angle))) - size / 2;
        think_ticks_[i] = kit::Box(think_, x, y, size, size, LV_RADIUS_CIRCLE, kTickColors[0]);
    }
    // Anillos que giran a distintas velocidades y en sentidos opuestos
    think_arcs_[0] = MakeArc(think_, 86, 3, kCyan, 0, 60);
    think_arcs_[1] = MakeArc(think_, 86, 3, kCyan, 180, 240);
    think_arcs_[2] = MakeArc(think_, 70, 6, kViolet, 0, 230);
    think_arcs_[3] = MakeArc(think_, 48, 2, 0xE6F7FF, 0, 100);
    // Partículas en órbita
    for (int i = 0; i < 3; ++i) {
        think_orbit_[i] = kit::Box(think_, kCX, kCY, 6, 6, LV_RADIUS_CIRCLE, kCyan);
    }
    // Núcleo que late con el tiempo que lleva pensando
    think_core_ = kit::Box(think_, kCX - 22, kCY - 22, 44, 44, LV_RADIUS_CIRCLE, kCyan);
    lv_obj_set_style_shadow_width(think_core_, 26, 0);
    lv_obj_set_style_shadow_spread(think_core_, 2, 0);
    lv_obj_set_style_shadow_color(think_core_, lv_color_hex(kCyan), 0);
    think_time_ = kit::Text(think_core_, 0, 13, 44, &font_mono_14, 0x00262E);
}

void SparkBotDisplay::ShowThinking(bool show) {
    think_visible_ = show;
    for (int i = 0; i < 2; ++i) {
        if (show) {
            lv_obj_add_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 7; ++i) {
        lv_obj_add_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (show) {
        if (!thinking_) {
            think_start_ms_ = lv_tick_get();
        }
        lv_obj_remove_flag(think_, LV_OBJ_FLAG_HIDDEN);
        kit::Loop(think_arcs_[0], SetArcRotation, 0, 360, 3200);
        kit::Loop(think_arcs_[1], SetArcRotation, 0, 360, 3200);
        kit::Loop(think_arcs_[2], SetArcRotation, 360, 0, 1900);
        kit::Loop(think_arcs_[3], SetArcRotation, 0, 360, 900);
        kit::Loop(think_core_, SetShadowOpa, 40, 220, 650, 0, true);
    } else {
        for (int i = 0; i < 4; ++i) {
            lv_anim_delete(think_arcs_[i], SetArcRotation);
        }
        lv_anim_delete(think_core_, SetShadowOpa);
        lv_obj_add_flag(think_, LV_OBJ_FLAG_HIDDEN);
        ApplyShape();
        last_color_ = 0xFFFFFFFF;
    }
}

// Un fotograma del reactor (25 por segundo)
void SparkBotDisplay::AnimateThinking() {
    const int head = static_cast<int>(ticks_ % kTicks);
    for (int i = 0; i < kTicks; ++i) {
        const int behind = (head - i + kTicks) % kTicks;
        const uint8_t level = behind == 0 ? 3 : (behind <= 3 ? 2 : (behind <= 9 ? 1 : 0));
        if (level != think_tick_level_[i]) {
            think_tick_level_[i] = level;
            lv_obj_set_style_bg_color(think_ticks_[i], lv_color_hex(kTickColors[level]), 0);
        }
    }
    for (int i = 0; i < 3; ++i) {
        const double angle = ticks_ * 0.13 + i * 2.0 * M_PI / 3;
        lv_obj_set_pos(think_orbit_[i], kCX + static_cast<int>(std::lround(kOrbitRadius * std::cos(angle))) - 3,
                       kCY + static_cast<int>(std::lround(kOrbitRadius * std::sin(angle))) - 3);
    }
    if (ticks_ % 3 == 0) {
        const uint32_t tenths = lv_tick_elaps(think_start_ms_) / 100;
        char text[24];
        snprintf(text, sizeof(text), "%u.%u", static_cast<unsigned>(std::min<uint32_t>(tenths / 10, 99)),
                 static_cast<unsigned>(tenths % 10));
        lv_label_set_text(think_time_, text);
    }
}

void SparkBotDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    if (role == nullptr) {
        return;
    }
    const bool has_text = content != nullptr && content[0] != '\0';
    if (strcmp(role, "user") == 0 && has_text) {
        Companion::Get().AddConversation();
    }
    DisplayLockGuard lock(this);
    if (strcmp(role, "user") == 0 && has_text) {
        // Ya ha entendido lo que has dicho: ahora piensa hasta que empiece a responder
        thinking_ = true;
        think_start_ms_ = lv_tick_get();
    } else if (strcmp(role, "assistant") == 0 && has_text) {
        thinking_ = false;
    }
}

bool SparkBotDisplay::SetEmotion(const char* emotion) {
    SpiLcdDisplay::SetEmotion(emotion);
    const std::string e = emotion != nullptr ? emotion : "";

    Shape shape = Shape::kNormal;
    uint32_t color = kCyan;
    if (Is(e, {"happy", "laughing", "funny", "silly", "delicious", "confident", "cool", "relaxed"})) {
        shape = Shape::kHappy;
    } else if (Is(e, {"loving", "kissy"})) {
        shape = Shape::kHappy;
        color = 0xFF7FC8;
    } else if (Is(e, {"embarrassed"})) {
        shape = Shape::kHappy;
        color = 0xFF9AB8;
    } else if (Is(e, {"sad", "crying"})) {
        shape = Shape::kSad;
        color = 0x5C9DFF;
    } else if (Is(e, {"angry"})) {
        shape = Shape::kAngry;
        color = 0xFF4A4A;
    } else if (Is(e, {"surprised", "shocked"})) {
        shape = Shape::kSurprised;
        color = 0xFFE082;
    } else if (Is(e, {"sleepy"})) {
        shape = Shape::kSleepy;
        color = 0x3A8FA8;
    } else if (Is(e, {"thinking", "confused"})) {
        shape = Shape::kThinking;
        color = 0xB388FF;
    } else if (Is(e, {"winking"})) {
        shape = Shape::kWink;
    }

    DisplayLockGuard lock(this);
    shape_ = shape;
    emotion_color_ = color;
    if (emoji_box_ != nullptr) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (face_ != nullptr) {
        ApplyShape();
    }
    return true;
}

// Coloca ojos y párpados según la expresión actual (con la pantalla bloqueada)
void SparkBotDisplay::ApplyShape() {
    for (int i = 0; i < 2; ++i) {
        int w = 50, h = 66, radius = 18;
        switch (shape_) {
            case Shape::kSurprised: w = 60; h = 60; radius = LV_RADIUS_CIRCLE; break;
            case Shape::kSleepy: w = 52; h = 10; radius = 5; break;
            case Shape::kSad: h = 56; break;
            case Shape::kThinking: if (i == 0) { h = 54; } break;
            case Shape::kWink: if (i == 1) { w = 52; h = 10; radius = 5; } break;
            default: break;
        }
        if (i == 0) {
            eye_w_ = w;
            eye_h_ = h;
        }
        lv_obj_set_size(eye_[i], w, h);
        lv_obj_set_style_radius(eye_[i], radius, 0);
        lv_obj_set_style_bg_color(eye_[i], lv_color_hex(emotion_color_), 0);

        lv_obj_t* top = lid_top_[i];
        lv_obj_t* bottom = lid_bottom_[i];
        lv_obj_add_flag(top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_transform_rotation(top, 0, 0);

        if (shape_ == Shape::kHappy) {
            // Párpado inferior redondo: ojos sonrientes en forma de media luna
            lv_obj_set_size(bottom, w + 24, h);
            lv_obj_set_style_radius(bottom, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_pos(bottom, -12, h * 55 / 100);
            lv_obj_remove_flag(bottom, LV_OBJ_FLAG_HIDDEN);
        } else if (shape_ == Shape::kSad || shape_ == Shape::kAngry) {
            // Párpado superior inclinado: tristeza o enfado según el sentido
            const int lid_w = w + 30;
            const int lid_h = h * 45 / 100;
            lv_obj_set_size(top, lid_w, lid_h);
            lv_obj_set_style_radius(top, 0, 0);
            lv_obj_set_pos(top, -15, -lid_h / 3);
            lv_obj_set_style_transform_pivot_x(top, lid_w / 2, 0);
            lv_obj_set_style_transform_pivot_y(top, lid_h / 2, 0);
            int angle = shape_ == Shape::kAngry ? 160 : -160;  // décimas de grado
            lv_obj_set_style_transform_rotation(top, i == 0 ? angle : -angle, 0);
            lv_obj_remove_flag(top, LV_OBJ_FLAG_HIDDEN);
        }
    }
    last_color_ = emotion_color_;
}

void SparkBotDisplay::TimerCallback(lv_timer_t* timer) {
    static_cast<SparkBotDisplay*>(lv_timer_get_user_data(timer))->OnTimer();
}

// Se ejecuta 25 veces por segundo dentro de LVGL (ya con la pantalla bloqueada)
void SparkBotDisplay::OnTimer() {
    if (face_ == nullptr) {
        return;
    }
    ticks_++;
    if (emoji_box_ != nullptr && !lv_obj_has_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (boot_ != nullptr && AnimateBoot(static_cast<int>(Application::GetInstance().GetDeviceState()))) {
        return;
    }
    if (levelup_ticks_ > 0) {
        AnimateLevelUp();
    }

    const DeviceState state = Application::GetInstance().GetDeviceState();
    const bool listening = state == kDeviceStateListening;
    const bool speaking = state == kDeviceStateSpeaking;
    const bool busy = state == kDeviceStateConnecting || state == kDeviceStateStarting ||
                      state == kDeviceStateActivating || state == kDeviceStateUpgrading;

    // Pensando: desde que entiende tu frase hasta que empieza a hablar (o conectando)
    if (state == kDeviceStateIdle || speaking || (thinking_ && lv_tick_elaps(think_start_ms_) > 30000)) {
        thinking_ = false;
    }
    const bool want_thinking = thinking_ || state == kDeviceStateConnecting;
    if (want_thinking != think_visible_) {
        ShowThinking(want_thinking);
    }
    UpdateClock(state == kDeviceStateIdle && !want_thinking);
    // Ojo: mientras arranca, la placa aún se está creando y no se le puede preguntar nada
    const bool board_ready = state != kDeviceStateUnknown && state != kDeviceStateStarting;
    if (board_ready && ticks_ % 50 == 1) {
        UpdateStatusIcons();
        UpdateFooter();
    }
    if (think_visible_) {
        AnimateThinking();
        return;
    }

    // Color: verde cuando te escucha, ámbar cuando está ocupado, y si no el de la emoción
    uint32_t color = listening ? kListen : (busy ? kBusy : emotion_color_);
    if (color != last_color_) {
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_bg_color(eye_[i], lv_color_hex(color), 0);
        }
        for (int i = 0; i < 7; ++i) {
            lv_obj_set_style_bg_color(mouth_[i], lv_color_hex(color), 0);
        }
        last_color_ = color;
    }

    // Mirada: cuando te escucha te mira de frente; si piensa, mira arriba a la derecha
    if (ticks_ >= next_gaze_) {
        if (listening) {
            target_x_ = 0;
            target_y_ = 0;
        } else if (shape_ == Shape::kThinking) {
            target_x_ = 14;
            target_y_ = -10;
        } else {
            target_x_ = static_cast<float>(RandomRange(0, 28)) - 14.0f;
            target_y_ = static_cast<float>(RandomRange(0, 12)) - 6.0f;
        }
        next_gaze_ = ticks_ + RandomRange(40, 110);
    }
    const int old_x = static_cast<int>(std::lround(gaze_x_));
    const int old_y = static_cast<int>(std::lround(gaze_y_));
    gaze_x_ += (target_x_ - gaze_x_) * 0.25f;
    gaze_y_ += (target_y_ - gaze_y_) * 0.25f;
    const int new_x = static_cast<int>(std::lround(gaze_x_));
    const int new_y = static_cast<int>(std::lround(gaze_y_));
    if (new_x != old_x || new_y != old_y) {
        for (int i = 0; i < 2; ++i) {
            lv_obj_set_style_translate_x(eye_[i], new_x, 0);
            lv_obj_set_style_translate_y(eye_[i], new_y, 0);
        }
    }

    // Parpadeo cada 3-5 segundos (no si tiene sueño)
    if (shape_ != Shape::kSleepy) {
        if (blink_step_ < 0 && ticks_ >= next_blink_) {
            blink_step_ = 0;
        }
        if (blink_step_ >= 0) {
            static const int kBlink[6] = {60, 20, 10, 20, 60, 100};  // % de la altura
            const int h = std::max(6, eye_h_ * kBlink[blink_step_] / 100);
            for (int i = 0; i < 2; ++i) {
                lv_obj_add_flag(lid_top_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lid_bottom_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(eye_[i], h);
            }
            blink_step_++;
            if (blink_step_ >= 6) {
                blink_step_ = -1;
                ApplyShape();
                last_color_ = 0xFFFFFFFF;  // fuerza a repintar el color en el siguiente ciclo
                // A veces parpadea dos veces seguidas
                next_blink_ = ticks_ + (RandomRange(0, 4) == 0 ? 6 : RandomRange(75, 125));
            }
        }
    }

    // Boca: barras que se mueven mientras habla
    if (speaking) {
        if (ticks_ % 2 == 0) {
            for (int i = 0; i < 7; ++i) {
                lv_obj_remove_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(mouth_[i], static_cast<int32_t>(RandomRange(4, 22)));
            }
        }
    } else if (!lv_obj_has_flag(mouth_[0], LV_OBJ_FLAG_HIDDEN)) {
        for (int i = 0; i < 7; ++i) {
            lv_obj_add_flag(mouth_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
