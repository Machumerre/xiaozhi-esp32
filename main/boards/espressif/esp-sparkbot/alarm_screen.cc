// Alarmas con amanecer animado y resumen de buenos días para el ESP-SparkBot.
#include "alarm_screen.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display/display.h"
#include "companion.h"
#include "mcp_server.h"
#include "screen_kit.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>

#define TAG "Alarms"

namespace {

constexpr size_t kMaxAlarms = 10;
constexpr int kMaxRingSeconds = 5 * 60;   // si nadie la apaga, se calla a los 5 minutos
constexpr int kSoundEverySeconds = 2;

const char* kDayShort[7] = {"lun", "mar", "mié", "jue", "vie", "sáb", "dom"};
const char* kDayKeys[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};

bool LocalNow(struct tm& local) {
    time_t now = time(nullptr);
    localtime_r(&now, &local);
    return local.tm_year >= 2025 - 1900;
}

uint32_t Lerp(uint32_t a, uint32_t b, int32_t t /* 0..1000 */) {
    auto channel = [&](int shift) {
        int ca = (a >> shift) & 0xFF;
        int cb = (b >> shift) & 0xFF;
        return static_cast<uint32_t>(ca + (cb - ca) * t / 1000) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

std::string Lower(std::string text) {
    for (auto& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

}  // namespace

AlarmScreen::AlarmScreen(Display* display) : display_(display) {
    Load();
    esp_timer_create_args_t args = {};
    args.callback = &AlarmScreen::TickCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "alarm_tick";
    args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1000000));
}

// ---------------------------------------------------------------------------
// Guardado: una alarma por línea "HH:MM|días|resumen|etiqueta"
// ---------------------------------------------------------------------------

void AlarmScreen::Load() {
    Settings settings("alarms", false);
    std::istringstream lines(settings.GetString("list", ""));
    std::string line;
    alarms_.clear();
    while (std::getline(lines, line) && alarms_.size() < kMaxAlarms) {
        Alarm alarm;
        int days = 0, briefing = 1;
        char label[64] = {};
        if (sscanf(line.c_str(), "%d:%d|%d|%d|%63[^\n]", &alarm.hour, &alarm.minute, &days, &briefing, label) >= 4) {
            alarm.days = static_cast<uint8_t>(days & 0x7F);
            alarm.briefing = briefing != 0;
            alarm.label = label;
            alarms_.push_back(alarm);
        }
    }
}

void AlarmScreen::Save() {
    std::string raw;
    char line[100];
    for (const auto& alarm : alarms_) {
        snprintf(line, sizeof(line), "%02d:%02d|%d|%d|%s\n", alarm.hour, alarm.minute, alarm.days,
                 alarm.briefing ? 1 : 0, alarm.label.c_str());
        raw += line;
    }
    Settings settings("alarms", true);
    settings.SetString("list", raw);
}

bool AlarmScreen::ParseTime(const std::string& text, int& hour, int& minute) {
    hour = minute = -1;
    if (sscanf(text.c_str(), "%d:%d", &hour, &minute) != 2 && sscanf(text.c_str(), "%d.%d", &hour, &minute) != 2) {
        if (sscanf(text.c_str(), "%d", &hour) != 1) {
            return false;
        }
        minute = 0;
    }
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool AlarmScreen::ParseDays(const std::string& raw, uint8_t& days) {
    const std::string text = Lower(raw);
    days = 0;
    if (text.empty() || text == "once") return true;
    if (text == "daily" || text == "everyday") { days = 0x7F; return true; }
    if (text == "weekdays") { days = 0x1F; return true; }
    if (text == "weekends") { days = 0x60; return true; }
    for (int i = 0; i < 7; ++i) {
        if (text.find(kDayKeys[i]) != std::string::npos) {
            days |= 1 << i;
        }
    }
    return days != 0;
}

std::string AlarmScreen::DaysText(uint8_t days) {
    if (days == 0) return "una vez";
    if (days == 0x7F) return "todos los días";
    if (days == 0x1F) return "de lunes a viernes";
    if (days == 0x60) return "fines de semana";
    std::string out;
    for (int i = 0; i < 7; ++i) {
        if (days & (1 << i)) {
            if (!out.empty()) out += ", ";
            out += kDayShort[i];
        }
    }
    return out;
}

std::string AlarmScreen::ListText() {
    if (alarms_.empty()) {
        return "No hay alarmas.";
    }
    std::string out = "Alarmas: ";
    char line[120];
    for (const auto& alarm : alarms_) {
        snprintf(line, sizeof(line), "%02d:%02d (%s%s%s); ", alarm.hour, alarm.minute, DaysText(alarm.days).c_str(),
                 alarm.label.empty() ? "" : ", ", alarm.label.c_str());
        out += line;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Reloj interno: comprueba las alarmas y hace sonar la que toque
// ---------------------------------------------------------------------------

void AlarmScreen::TickCallback(void* arg) { static_cast<AlarmScreen*>(arg)->Tick(); }

void AlarmScreen::Tick() {
    auto& app = Application::GetInstance();
    struct tm local = {};
    const bool time_ok = LocalNow(local);

    bool play_sound = false;
    bool stop_because_talking = false;
    bool auto_stopped = false;
    bool start = false;
    Alarm to_ring;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (ringing_) {
            ring_seconds_++;
            // Si empiezas a hablarle, se calla (ya estás despierto)
            if (app.GetDeviceState() == kDeviceStateListening && ring_seconds_ > 2) {
                stop_because_talking = true;
            } else if (ring_seconds_ >= kMaxRingSeconds) {
                ringing_ = false;
                auto_stopped = true;
            } else {
                play_sound = ring_seconds_ % kSoundEverySeconds == 0;
            }
        }
        if (time_ok) {
            const int minute_key = local.tm_yday * 1440 + local.tm_hour * 60 + local.tm_min;
            if (minute_key != last_checked_minute_) {
                last_checked_minute_ = minute_key;
                const int weekday = (local.tm_wday + 6) % 7;  // lunes = 0
                for (size_t i = 0; i < alarms_.size(); ++i) {
                    const Alarm& alarm = alarms_[i];
                    if (alarm.hour != local.tm_hour || alarm.minute != local.tm_min) continue;
                    if (alarm.days != 0 && !(alarm.days & (1 << weekday))) continue;
                    to_ring = alarm;
                    start = !ringing_;
                    if (alarm.days == 0) {  // las de una vez se borran al sonar
                        alarms_.erase(alarms_.begin() + i);
                        Save();
                    }
                    break;
                }
            }
        }
    }

    if (stop_because_talking) {
        StopRinging(false);
        return;
    }
    if (start) {
        StartRinging(to_ring);
        return;
    }
    if (play_sound) {
        app.Schedule([]() { Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION); });
    }
    // A las 20:00, si la racha de estudio está en peligro, avisa una vez
    if (time_ok && local.tm_sec < 2 && Companion::Get().ShouldRemindStreak(local)) {
        const int streak = Companion::Get().GetSnapshot().streak;
        app.Schedule([this, streak]() {
            char text[64];
            snprintf(text, sizeof(text), "\u00A1No pierdas tu racha de %d d\u00EDas!", streak);
            display_->ShowNotification(text, 10000);
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        });
    }
    if (auto_stopped) {
        app.Schedule([this]() { ShowScreen(false); });
        return;
    }
    if (IsRinging()) {
        app.Schedule([this]() { UpdateTime(); });
    }
}

void AlarmScreen::StartRinging(const Alarm& alarm) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        ringing_ = true;
        ring_seconds_ = 0;
        ring_briefing_ = alarm.briefing;
        ring_label_ = alarm.label;
    }
    ESP_LOGI(TAG, "Sonando alarma de las %02d:%02d", alarm.hour, alarm.minute);
    auto& app = Application::GetInstance();
    app.Schedule([this]() {
        ShowScreen(true);
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_EXCLAMATION);
    });
}

bool AlarmScreen::IsRinging() {
    std::lock_guard<std::mutex> guard(mutex_);
    return ringing_;
}

void AlarmScreen::Dismiss(bool briefing) {
    if (IsRinging()) {
        Companion::Get().AddXp(5);  // levantarse a la primera da puntos
    }
    StopRinging(briefing);
}

void AlarmScreen::StopRinging(bool briefing) {
    bool want_briefing = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!ringing_) {
            return;
        }
        ringing_ = false;
        want_briefing = briefing && ring_briefing_;
    }
    auto& app = Application::GetInstance();
    app.Schedule([this]() { ShowScreen(false); });
    if (want_briefing) {
        // Le pasamos a la IA la petición como si se la dijeras tú al despertar
        app.WakeWordInvoke(
            "Acaba de sonar mi alarma y ya la he apagado. Dame los buenos días como Jarvis y un resumen "
            "corto del día: dime la fecha, enséñame el tiempo de hoy en pantalla con self.weather.show y "
            "dime cuánto falta para mi próximo examen con self.exams.show_next.");
    }
}

// ---------------------------------------------------------------------------
// Pantalla: amanecer con el sol saliendo, la hora grande y la etiqueta
// ---------------------------------------------------------------------------

void AlarmScreen::SetSunrise(void* obj, int32_t progress) {
    auto* self = static_cast<AlarmScreen*>(obj);
    if (self->sky_ == nullptr) {
        return;
    }
    // El cielo pasa de noche a amanecer y el sol sube por detrás del horizonte
    lv_obj_set_style_bg_color(self->sky_, lv_color_hex(Lerp(0x050A1C, 0x1F3C78, progress)), 0);
    lv_obj_set_style_bg_grad_color(self->sky_, lv_color_hex(Lerp(0x14102A, 0xFF8A3D, progress)), 0);
    lv_obj_set_y(self->sun_, 230 - 80 * progress / 1000);
    lv_obj_set_style_bg_color(self->sun_, lv_color_hex(Lerp(0xC2410C, 0xFFC93C, progress)), 0);
    lv_obj_set_style_shadow_opa(self->sun_, static_cast<lv_opa_t>(60 + 160 * progress / 1000), 0);
}

void AlarmScreen::Build() {
    if (root_ != nullptr) {
        return;
    }
    root_ = kit::Overlay();

    sky_ = kit::Box(root_, 0, 0, 240, 240, 0, 0x050A1C);
    lv_obj_set_style_bg_grad_dir(sky_, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_grad_color(sky_, lv_color_hex(0x14102A), 0);

    sun_ = kit::Box(root_, 75, 230, 90, 90, LV_RADIUS_CIRCLE, 0xC2410C);
    lv_obj_set_style_shadow_width(sun_, 40, 0);
    lv_obj_set_style_shadow_spread(sun_, 6, 0);
    lv_obj_set_style_shadow_color(sun_, lv_color_hex(0xFF8A3D), 0);

    // Suelo con la línea del horizonte tipo visor
    lv_obj_t* ground = kit::Box(root_, 0, 192, 240, 48, 0, 0x04060C);
    lv_obj_set_style_border_side(ground, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(ground, 2, 0);
    lv_obj_set_style_border_color(ground, lv_color_hex(0xFFB36B), 0);

    kit::Corners(root_, 0xFFD9A8);
    title_label_ = kit::Centered(root_, 22, &font_mono_14, 0xFFE6C7, 200);
    lv_obj_set_style_text_letter_space(title_label_, 2, 0);
    time_label_ = kit::Centered(root_, 40, &font_mono_56, 0xFFFFFF, 220);
    lv_obj_set_style_text_letter_space(time_label_, -2, 0);
    hint_label_ = kit::Centered(root_, 206, &font_mono_14, 0xFFB36B, 220);
    lv_label_set_text(hint_label_, "PULSA EL BOTÓN");
    kit::Loop(hint_label_, kit::SetOpacity, 60, 255, 700, 0, true);
}

void AlarmScreen::ShowScreen(bool show) {
    DisplayLockGuard lock(display_);
    if (!show) {
        if (root_ != nullptr && !lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
            lv_anim_delete(this, &AlarmScreen::SetSunrise);
            lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    Build();
    std::string title;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        title = ring_label_.empty() ? std::string("▸ BUENOS DÍAS") : "▸ " + kit::ScreenText(ring_label_, 22);
    }
    lv_label_set_text(title_label_, title.c_str());

    // Amanecer de 20 segundos
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, &AlarmScreen::SetSunrise);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 20000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    SetSunrise(this, 0);

    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    struct tm local = {};
    if (LocalNow(local)) {
        char text[24];
        snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        lv_label_set_text(time_label_, text);
    }
}

void AlarmScreen::UpdateTime() {
    DisplayLockGuard lock(display_);
    if (root_ == nullptr || lv_obj_has_flag(root_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    struct tm local = {};
    if (!LocalNow(local)) {
        return;
    }
    char text[24];
    snprintf(text, sizeof(text), (local.tm_sec % 2 == 0) ? "%02d:%02d" : "%02d %02d", local.tm_hour, local.tm_min);
    lv_label_set_text(time_label_, text);
    lv_obj_move_foreground(root_);
}

// ---------------------------------------------------------------------------
// Herramientas para la IA
// ---------------------------------------------------------------------------

void AlarmScreen::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.alarm.set",
        "Pone una alarma en el robot. 'time' en formato HH:MM de 24 horas (hora de España). 'days': "
        "'once' (una vez, la próxima vez que llegue esa hora), 'daily', 'weekdays' (lunes a viernes), "
        "'weekends', o días en inglés separados por comas (p. ej. 'mon,wed,fri'). 'label' es opcional "
        "(p. ej. 'Gimnasio'). Con 'briefing' true, al apagarla dará los buenos días con el tiempo y el "
        "próximo examen. Set an alarm.",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("days", kPropertyTypeString, std::string("once")),
            Property("label", kPropertyTypeString, std::string("")),
            Property("briefing", kPropertyTypeBoolean, true),
        }),
        [this](const PropertyList& p) -> ToolResult {
            Alarm alarm;
            if (!ParseTime(p["time"].value<std::string>(), alarm.hour, alarm.minute)) {
                return std::unexpected(std::string("Hora no válida; usa HH:MM, por ejemplo 07:30"));
            }
            if (!ParseDays(p["days"].value<std::string>(), alarm.days)) {
                return std::unexpected(std::string("Días no válidos; usa once, daily, weekdays, weekends o mon,tue..."));
            }
            alarm.label = p["label"].value<std::string>();
            alarm.label.erase(std::remove(alarm.label.begin(), alarm.label.end(), '|'), alarm.label.end());
            alarm.label.erase(std::remove(alarm.label.begin(), alarm.label.end(), '\n'), alarm.label.end());
            alarm.label = alarm.label.substr(0, 40);
            alarm.briefing = p["briefing"].value<bool>();

            std::lock_guard<std::mutex> guard(mutex_);
            // Si ya hay una a la misma hora, se sustituye
            alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                         [&](const Alarm& a) { return a.hour == alarm.hour && a.minute == alarm.minute; }),
                          alarms_.end());
            if (alarms_.size() >= kMaxAlarms) {
                return std::unexpected(std::string("Ya hay 10 alarmas; borra alguna antes"));
            }
            alarms_.push_back(alarm);
            std::sort(alarms_.begin(), alarms_.end(), [](const Alarm& a, const Alarm& b) {
                return a.hour * 60 + a.minute < b.hour * 60 + b.minute;
            });
            Save();
            char text[160];
            snprintf(text, sizeof(text), "Alarma puesta a las %02d:%02d, %s.", alarm.hour, alarm.minute,
                     DaysText(alarm.days).c_str());
            struct tm local = {};
            if (!LocalNow(local)) {
                return std::string(text) + " Aviso: el robot aún no tiene la hora sincronizada.";
            }
            return std::string(text);
        });

    mcp.AddTool("self.alarm.list", "Lista las alarmas guardadas. List alarms.", PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    std::lock_guard<std::mutex> guard(mutex_);
                    return ListText();
                });

    mcp.AddTool(
        "self.alarm.remove",
        "Borra la alarma de la hora indicada ('time' HH:MM) o todas con 'all'. Remove alarms.",
        PropertyList({Property("time", kPropertyTypeString)}),
        [this](const PropertyList& p) -> ToolResult {
            const std::string time_text = Lower(p["time"].value<std::string>());
            std::lock_guard<std::mutex> guard(mutex_);
            if (time_text == "all" || time_text == "todas") {
                alarms_.clear();
                Save();
                return std::string("Todas las alarmas borradas.");
            }
            int hour = 0, minute = 0;
            if (!ParseTime(time_text, hour, minute)) {
                return std::unexpected(std::string("Hora no válida"));
            }
            const size_t before = alarms_.size();
            alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                         [&](const Alarm& a) { return a.hour == hour && a.minute == minute; }),
                          alarms_.end());
            if (alarms_.size() == before) {
                return std::unexpected(std::string("No hay ninguna alarma a esa hora"));
            }
            Save();
            return std::string("Alarma borrada. ") + ListText();
        });

    mcp.AddTool(
        "self.alarm.stop",
        "Apaga la alarma que está sonando. Úsalo si el usuario dice que la apagues. Stop the ringing alarm.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            if (!IsRinging()) {
                return std::string("No hay ninguna alarma sonando.");
            }
            StopRinging(false);
            return std::string("Alarma apagada.");
        });

    mcp.AddTool(
        "self.alarm.snooze",
        "Pospone la alarma que suena unos minutos (por defecto 9). Snooze the alarm.",
        PropertyList({Property("minutes", kPropertyTypeInteger, 9, 1, 60)}),
        [this](const PropertyList& p) -> ToolResult {
            const int minutes = p["minutes"].value<int>();
            struct tm local = {};
            if (!LocalNow(local)) {
                return std::unexpected(std::string("El robot no tiene la hora sincronizada"));
            }
            StopRinging(false);
            Alarm alarm;
            const int total = (local.tm_hour * 60 + local.tm_min + minutes) % 1440;
            alarm.hour = total / 60;
            alarm.minute = total % 60;
            alarm.days = 0;
            alarm.briefing = true;
            alarm.label = "Otra vez";
            std::lock_guard<std::mutex> guard(mutex_);
            alarms_.push_back(alarm);
            Save();
            char text[80];
            snprintf(text, sizeof(text), "Pospuesta %d minutos, sonará a las %02d:%02d.", minutes, alarm.hour,
                     alarm.minute);
            return std::string(text);
        });

    ESP_LOGI(TAG, "Herramientas de alarma registradas (%u alarmas)", static_cast<unsigned>(alarms_.size()));
}
