// Agenda del SparkBot: eventos con recordatorio, vista de mes y próximos días (incluye exámenes).
#include "agenda_screen.h"

#include "application.h"
#include "assets/lang_config.h"
#include "display/display.h"
#include "mcp_server.h"
#include "screen_kit.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>

#define TAG "Agenda"

namespace {

constexpr size_t kMaxEvents = 40;
constexpr uint32_t kShowMs = 20000;
constexpr int kRemindBeforeMin = 10;

const char* kMonthNames[12] = {"ENERO", "FEBRERO", "MARZO", "ABRIL", "MAYO", "JUNIO",
                               "JULIO", "AGOSTO", "SEPTIEMBRE", "OCTUBRE", "NOVIEMBRE", "DICIEMBRE"};
const char* kDayShort[7] = {"LUN", "MAR", "MIÉ", "JUE", "VIE", "SÁB", "DOM"};
const char* kDaySpoken[7] = {"lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo"};
const char* kDayLetters[7] = {"L", "M", "X", "J", "V", "S", "D"};

constexpr uint32_t kEventColor = 0xFFB300;
constexpr uint32_t kExamColor = 0xFF6E6E;

int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int Weekday(int64_t days) { return static_cast<int>(((days % 7) + 7 + 3) % 7); }  // lunes = 0

int DaysInMonth(int y, int m) {
    return static_cast<int>(DaysFromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - DaysFromCivil(y, m, 1));
}

bool Today(struct tm& local, int64_t& days) {
    time_t now = time(nullptr);
    localtime_r(&now, &local);
    if (local.tm_year < 2025 - 1900) {
        return false;
    }
    days = DaysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return true;
}

bool ParseDate(const std::string& text, int& y, int& m, int& d) {
    return sscanf(text.c_str(), "%d-%d-%d", &y, &m, &d) == 3 && y >= 2020 && m >= 1 && m <= 12 && d >= 1 &&
           d <= DaysInMonth(y, m);
}

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string Clean(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '|' || c == '\n'; }), text.end());
    return text.substr(0, 48);
}

}  // namespace

int64_t AgendaScreen::Event::Days() const { return DaysFromCivil(year, month, day); }

AgendaScreen::AgendaScreen(Display* display) : display_(display) {
    Load();
    esp_timer_create_args_t args = {};
    args.callback = &AgendaScreen::TickCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "agenda_tick";
    args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 20 * 1000000ULL));
}

// ---------------------------------------------------------------------------
// Datos: "AAAA-MM-DD|HH:MM|título" (HH:MM vacío = todo el día)
// ---------------------------------------------------------------------------

void AgendaScreen::Load() {
    Settings settings("agenda", false);
    std::istringstream lines(settings.GetString("events", ""));
    std::string line;
    events_.clear();
    while (std::getline(lines, line)) {
        const size_t a = line.find('|');
        const size_t b = a == std::string::npos ? a : line.find('|', a + 1);
        if (b == std::string::npos) continue;
        Event e;
        if (!ParseDate(line.substr(0, a), e.year, e.month, e.day)) continue;
        const std::string time_text = line.substr(a + 1, b - a - 1);
        if (sscanf(time_text.c_str(), "%d:%d", &e.hour, &e.minute) != 2) e.hour = -1;
        e.title = line.substr(b + 1);
        events_.push_back(e);
    }
}

void AgendaScreen::Save() {
    // Se borran los eventos de hace más de un día
    struct tm local = {};
    int64_t today = 0;
    if (Today(local, today)) {
        events_.erase(std::remove_if(events_.begin(), events_.end(), [&](const Event& e) { return e.Days() < today - 1; }),
                      events_.end());
    }
    std::sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
        return a.Days() * 1440 + a.hour * 60 + a.minute < b.Days() * 1440 + b.hour * 60 + b.minute;
    });
    std::string raw;
    char line[96];
    for (const auto& e : events_) {
        char time_text[24] = "";
        if (e.hour >= 0) snprintf(time_text, sizeof(time_text), "%02d:%02d", e.hour, e.minute);
        snprintf(line, sizeof(line), "%04d-%02d-%02d|%s|%s\n", e.year, e.month, e.day, time_text, e.title.c_str());
        raw += line;
    }
    Settings settings("agenda", true);
    settings.SetString("events", raw);
}

// Eventos + exámenes guardados por la pantalla de estudio
std::vector<AgendaScreen::Event> AgendaScreen::WithExams() {
    std::vector<Event> all = events_;
    Settings settings("study", false);
    std::istringstream lines(settings.GetString("exams", ""));
    std::string line;
    while (std::getline(lines, line)) {
        const size_t bar = line.find('|');
        Event e;
        if (bar == std::string::npos || !ParseDate(line.substr(0, bar), e.year, e.month, e.day)) continue;
        e.title = line.substr(bar + 1);
        e.exam = true;
        all.push_back(e);
    }
    std::sort(all.begin(), all.end(), [](const Event& a, const Event& b) {
        return a.Days() * 1440 + a.hour * 60 + a.minute < b.Days() * 1440 + b.hour * 60 + b.minute;
    });
    return all;
}

std::string AgendaScreen::EventText(const Event& e, bool with_date) {
    std::string out;
    char text[48];
    if (with_date) {
        snprintf(text, sizeof(text), "%s %d/%d", kDaySpoken[Weekday(e.Days())], e.day, e.month);
        out += text;
    }
    if (e.hour >= 0) {
        snprintf(text, sizeof(text), "%sa las %02d:%02d", with_date ? " " : "", e.hour, e.minute);
        out += text;
    }
    out += ": ";
    if (e.exam) out += "EXAMEN de ";
    out += e.title;
    return out;
}

// ---------------------------------------------------------------------------
// Recordatorio 10 minutos antes de cada evento con hora
// ---------------------------------------------------------------------------

void AgendaScreen::TickCallback(void* arg) { static_cast<AgendaScreen*>(arg)->Tick(); }

void AgendaScreen::Tick() {
    struct tm local = {};
    int64_t today = 0;
    if (!Today(local, today)) {
        return;
    }
    const int now_min = local.tm_hour * 60 + local.tm_min;
    std::string message;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const auto& e : events_) {
            if (e.hour < 0 || e.Days() != today) continue;
            const int diff = e.hour * 60 + e.minute - now_min;
            if (diff < 0 || diff > kRemindBeforeMin) continue;
            char key[80];
            snprintf(key, sizeof(key), "%lld|%d|%s", static_cast<long long>(today), e.hour * 60 + e.minute,
                     e.title.c_str());
            if (!reminded_.insert(key).second) continue;
            char text[80];
            snprintf(text, sizeof(text), "%02d:%02d %s", e.hour, e.minute, kit::ScreenText(e.title, 30).c_str());
            message = text;
            break;
        }
        if (reminded_.size() > 64) reminded_.clear();
    }
    if (message.empty()) {
        return;
    }
    ESP_LOGI(TAG, "Recordatorio: %s", message.c_str());
    Application::GetInstance().Schedule([this, message]() {
        display_->ShowNotification(message, 15000);
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
    });
}

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------

void AgendaScreen::Build() {
    if (root_ != nullptr) {
        return;
    }
    root_ = kit::Overlay();
    kit::Corners(root_);
    title_label_ = kit::Text(root_, 18, 8, 150, &font_mono_14, kit::kHudCyan, LV_TEXT_ALIGN_LEFT);
    right_label_ = kit::Text(root_, 160, 8, 62, &font_mono_14, kit::kHudDim, LV_TEXT_ALIGN_RIGHT);
    kit::HLine(root_, 18, 30, 204);

    // Vista de mes: 7 columnas x 6 filas
    month_view_ = kit::Group(root_, 0, 0, 240, 240);
    for (int c = 0; c < 7; ++c) {
        lv_obj_t* letter = kit::Text(month_view_, 8 + c * 32, 36, 32, &font_mono_14, c >= 5 ? 0x2E6676 : kit::kHudDim);
        lv_label_set_text(letter, kDayLetters[c]);
    }
    for (int i = 0; i < 42; ++i) {
        const int x = 8 + (i % 7) * 32;
        const int y = 56 + (i / 7) * 29;
        cell_bg_[i] = kit::Box(month_view_, x + 4, y, 24, 24, LV_RADIUS_CIRCLE, 0x000000);
        cell_label_[i] = kit::Text(month_view_, x, y + 3, 32, &font_mono_14, 0xE6F7FF);
        cell_dot_[i] = kit::Box(month_view_, x + 14, y + 23, 4, 4, LV_RADIUS_CIRCLE, kEventColor);
    }

    // Vista de lista: próximos días
    list_view_ = kit::Group(root_, 0, 0, 240, 240);
    for (int i = 0; i < 6; ++i) {
        const int y = 42 + i * 30;
        row_date_[i] = kit::Text(list_view_, 14, y, 54, &font_mono_14, kit::kHudDim, LV_TEXT_ALIGN_LEFT);
        row_time_[i] = kit::Text(list_view_, 68, y, 46, &font_mono_14, kit::kHudCyan, LV_TEXT_ALIGN_LEFT);
        row_title_[i] = kit::Text(list_view_, 116, y, 112, &font_mono_14, 0xFFFFFF, LV_TEXT_ALIGN_LEFT,
                                  LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        if (i < 5) kit::HLine(list_view_, 14, y + 23, 212, 0x0F2A33);
    }
    empty_label_ = kit::Centered(list_view_, 110, &font_mono_20, kit::kHudDim);
    lv_label_set_text(empty_label_, "Nada a la vista");

    hide_timer_ = lv_timer_create(&AgendaScreen::HideCallback, 500, this);
}

void AgendaScreen::Reveal(bool month) {
    if (month) {
        lv_obj_remove_flag(month_view_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(month_view_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(list_view_, LV_OBJ_FLAG_HIDDEN);
    }
    visible_until_ = lv_tick_get() + kShowMs;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void AgendaScreen::HideCallback(lv_timer_t* timer) {
    auto* self = static_cast<AgendaScreen*>(lv_timer_get_user_data(timer));
    if (self->root_ != nullptr && !lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN) &&
        static_cast<int32_t>(lv_tick_get() - self->visible_until_) >= 0) {
        lv_obj_add_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void AgendaScreen::ShowMonth(int year, int month, const std::vector<Event>& events) {
    DisplayLockGuard lock(display_);
    Build();
    char text[32];
    snprintf(text, sizeof(text), "▸ %s %d", kMonthNames[month - 1], year);
    lv_label_set_text(title_label_, text);

    struct tm local = {};
    int64_t today = -1;
    Today(local, today);
    int count = 0;
    for (const auto& e : events) {
        if (e.year == year && e.month == month) count++;
    }
    snprintf(text, sizeof(text), "%d EV", count);
    lv_label_set_text(right_label_, count > 0 ? text : "");

    const int64_t first = DaysFromCivil(year, month, 1);
    const int offset = Weekday(first);
    const int length = DaysInMonth(year, month);
    for (int i = 0; i < 42; ++i) {
        const int day = i - offset + 1;
        const bool valid = day >= 1 && day <= length;
        lv_obj_add_flag(cell_dot_[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(cell_bg_[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_border_width(cell_bg_[i], 0, 0);
        if (!valid) {
            lv_label_set_text(cell_label_[i], "");
            continue;
        }
        snprintf(text, sizeof(text), "%d", day);
        lv_label_set_text(cell_label_[i], text);
        const int64_t days = first + day - 1;
        const bool weekend = i % 7 >= 5;
        uint32_t color = days < today ? 0x3A5560 : (weekend ? 0x8FB8C4 : 0xE6F7FF);

        bool has_event = false, has_exam = false;
        for (const auto& e : events) {
            if (e.Days() == days) {
                (e.exam ? has_exam : has_event) = true;
            }
        }
        if (has_exam || has_event) {
            lv_obj_remove_flag(cell_dot_[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(cell_dot_[i], lv_color_hex(has_exam ? kExamColor : kEventColor), 0);
        }
        if (has_exam) {
            lv_obj_set_style_border_width(cell_bg_[i], 1, 0);
            lv_obj_set_style_border_color(cell_bg_[i], lv_color_hex(kExamColor), 0);
        }
        if (days == today) {
            lv_obj_set_style_bg_color(cell_bg_[i], lv_color_hex(kit::kHudCyan), 0);
            color = 0x001A20;
        }
        kit::SetColor(cell_label_[i], color);
    }
    Reveal(true);
}

void AgendaScreen::ShowUpcoming(const std::vector<Event>& events, int64_t today, int days) {
    DisplayLockGuard lock(display_);
    Build();
    char text[48];
    snprintf(text, sizeof(text), "▸ PRÓXIMOS %d DÍAS", days);
    lv_label_set_text(title_label_, text);
    lv_label_set_text(right_label_, "");

    int row = 0;
    int64_t last_day = -1;
    for (const auto& e : events) {
        if (row >= 6) break;
        const int64_t d = e.Days();
        if (d < today || d >= today + days) continue;
        if (d != last_day) {
            if (d == today) {
                snprintf(text, sizeof(text), "HOY");
            } else if (d == today + 1) {
                snprintf(text, sizeof(text), "MAÑANA");
            } else {
                snprintf(text, sizeof(text), "%s %d", kDayShort[Weekday(d)], e.day);
            }
            last_day = d;
        } else {
            text[0] = '\0';
        }
        lv_label_set_text(row_date_[row], text);
        if (e.hour >= 0) {
            snprintf(text, sizeof(text), "%02d:%02d", e.hour, e.minute);
        } else {
            snprintf(text, sizeof(text), e.exam ? "EXAM" : "—");
        }
        lv_label_set_text(row_time_[row], text);
        kit::SetColor(row_time_[row], e.exam ? kExamColor : kit::kHudCyan);
        const std::string title = kit::ScreenText(e.title, 40);
        lv_label_set_text(row_title_[row], title.c_str());
        kit::SetColor(row_title_[row], e.exam ? kExamColor : 0xFFFFFF);
        row++;
    }
    for (int i = row; i < 6; ++i) {
        lv_label_set_text(row_date_[i], "");
        lv_label_set_text(row_time_[i], "");
        lv_label_set_text(row_title_[i], "");
    }
    if (row == 0) {
        lv_obj_remove_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    }
    Reveal(false);
}

// ---------------------------------------------------------------------------
// Herramientas para la IA
// ---------------------------------------------------------------------------

void AgendaScreen::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.agenda.add",
        "Añade un evento a la agenda del robot (cita, quedada, clase, salida a la montaña...). 'date' en "
        "formato AAAA-MM-DD, 'time' opcional en HH:MM (vacío = todo el día), 'title' corto. Si tiene hora, "
        "el robot avisa 10 minutos antes. Para exámenes usa self.exams.add. Add a calendar event.",
        PropertyList({
            Property("date", kPropertyTypeString),
            Property("title", kPropertyTypeString),
            Property("time", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& p) -> ToolResult {
            Event e;
            if (!ParseDate(p["date"].value<std::string>(), e.year, e.month, e.day)) {
                return std::unexpected(std::string("Fecha no válida; usa AAAA-MM-DD"));
            }
            const std::string time_text = p["time"].value<std::string>();
            if (!time_text.empty()) {
                if (sscanf(time_text.c_str(), "%d:%d", &e.hour, &e.minute) != 2 || e.hour < 0 || e.hour > 23 ||
                    e.minute < 0 || e.minute > 59) {
                    return std::unexpected(std::string("Hora no válida; usa HH:MM"));
                }
            }
            e.title = Clean(p["title"].value<std::string>());
            if (e.title.empty()) {
                return std::unexpected(std::string("Falta el título"));
            }
            std::vector<Event> all;
            struct tm local = {};
            int64_t today = 0;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (events_.size() >= kMaxEvents) {
                    return std::unexpected(std::string("La agenda está llena (40 eventos)"));
                }
                events_.push_back(e);
                Save();
                all = WithExams();
            }
            if (Today(local, today)) {
                ShowUpcoming(all, std::min(today, e.Days()), std::max<int>(7, static_cast<int>(e.Days() - today) + 1));
            }
            return "Apuntado: " + EventText(e, true);
        });

    mcp.AddTool(
        "self.agenda.upcoming",
        "Muestra en pantalla y devuelve los eventos y exámenes de los próximos días ('days', por defecto "
        "7). Úsalo para '¿qué tengo hoy/mañana/esta semana?'. Show upcoming events.",
        PropertyList({Property("days", kPropertyTypeInteger, 7, 1, 60)}),
        [this](const PropertyList& p) -> ToolResult {
            const int days = p["days"].value<int>();
            struct tm local = {};
            int64_t today = 0;
            if (!Today(local, today)) {
                return std::unexpected(std::string("El robot aún no tiene la fecha sincronizada"));
            }
            std::vector<Event> all;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                all = WithExams();
            }
            ShowUpcoming(all, today, days);
            std::string out;
            for (const auto& e : all) {
                if (e.Days() >= today && e.Days() < today + days) {
                    out += EventText(e, true) + ". ";
                }
            }
            char head[80];
            snprintf(head, sizeof(head), "Hoy es %s %d/%d. ", kDaySpoken[Weekday(today)], local.tm_mday,
                     local.tm_mon + 1);
            return std::string(head) + (out.empty() ? "No hay nada en esos días." : out);
        });

    mcp.AddTool(
        "self.agenda.month",
        "Muestra el calendario del mes en la pantalla con los días que tienen eventos o exámenes. "
        "'offset' 0 = este mes, 1 = el siguiente, -1 = el anterior. Show the month calendar.",
        PropertyList({Property("offset", kPropertyTypeInteger, 0, -12, 12)}),
        [this](const PropertyList& p) -> ToolResult {
            struct tm local = {};
            int64_t today = 0;
            if (!Today(local, today)) {
                return std::unexpected(std::string("El robot aún no tiene la fecha sincronizada"));
            }
            int index = local.tm_year * 12 + local.tm_mon + p["offset"].value<int>();
            const int year = index / 12 + 1900;
            const int month = index % 12 + 1;
            std::vector<Event> all;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                all = WithExams();
            }
            ShowMonth(year, month, all);
            std::string out;
            for (const auto& e : all) {
                if (e.year == year && e.month == month) out += EventText(e, true) + ". ";
            }
            return std::string("Calendario mostrado. ") + (out.empty() ? "Ese mes no hay eventos." : out);
        });

    mcp.AddTool(
        "self.agenda.remove",
        "Borra eventos de la agenda de una fecha ('date' AAAA-MM-DD). Si das 'title', solo los que lo "
        "contengan. Remove calendar events.",
        PropertyList({
            Property("date", kPropertyTypeString),
            Property("title", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& p) -> ToolResult {
            int y = 0, m = 0, d = 0;
            if (!ParseDate(p["date"].value<std::string>(), y, m, d)) {
                return std::unexpected(std::string("Fecha no válida; usa AAAA-MM-DD"));
            }
            const std::string needle = Lower(p["title"].value<std::string>());
            std::lock_guard<std::mutex> guard(mutex_);
            const size_t before = events_.size();
            events_.erase(std::remove_if(events_.begin(), events_.end(),
                                         [&](const Event& e) {
                                             return e.year == y && e.month == m && e.day == d &&
                                                    (needle.empty() || Lower(e.title).find(needle) != std::string::npos);
                                         }),
                          events_.end());
            const size_t removed = before - events_.size();
            if (removed == 0) {
                return std::unexpected(std::string("No he encontrado ese evento"));
            }
            Save();
            return "Borrados " + std::to_string(removed) + " eventos.";
        });

    ESP_LOGI(TAG, "Agenda lista (%u eventos)", static_cast<unsigned>(events_.size()));
}
