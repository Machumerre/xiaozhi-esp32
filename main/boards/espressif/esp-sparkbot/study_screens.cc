// Pantallas propias para el ESP-SparkBot: Pomodoro y cuenta atrás de exámenes.
#include "study_screens.h"

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

#define TAG "StudyScreens"

namespace {

constexpr int64_t kSecondUs = 1000000;
constexpr int64_t kMinuteUs = 60 * kSecondUs;
constexpr int64_t kExamScreenUs = 15 * kSecondUs;
constexpr size_t kMaxExams = 12;

lv_color_t ColorStudy() { return lv_color_hex(0x00E676); }
lv_color_t ColorBreak() { return lv_color_hex(0x2F8CFF); }
lv_color_t ColorPause() { return lv_color_hex(0xB388FF); }
lv_color_t ColorExam() { return lv_color_hex(0xC3A6FF); }
lv_color_t ColorOff() { return lv_color_hex(0x16201B); }
lv_color_t ColorText() { return lv_color_hex(0xE6EEF0); }
lv_color_t ColorBlack() { return lv_color_hex(0x000000); }

const char* kMonths[12] = {"ene", "feb", "mar", "abr", "may", "jun",
                           "jul", "ago", "sep", "oct", "nov", "dic"};

lv_obj_t* CreateBox(lv_obj_t* parent, int x, int y, int w, int h, int radius, lv_color_t color) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t* CreateLabel(lv_obj_t* parent, int y, int width, lv_color_t color,
                      const lv_font_t* font = &font_mono_14) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* CreateRoot() {
    lv_obj_t* root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, ColorBlack(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    return root;
}

void SetVisible(lv_obj_t* obj, bool visible, bool bring_front = true) {
    if (obj == nullptr) {
        return;
    }
    if (visible) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            if (bring_front) {
                lv_obj_move_foreground(obj);
            }
        }
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

StudyScreens::StudyScreens(Display* display) : display_(display) {}

// ---------------------------------------------------------------------------
// Utilidades de texto y fechas
// ---------------------------------------------------------------------------

// Solo los caracteres que tiene la fuente de consola (con tildes y ñ)
std::string StudyScreens::ToScreenText(const std::string& utf8, size_t max_chars) {
    return kit::ScreenText(utf8, max_chars);
}

int64_t StudyScreens::DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool StudyScreens::TodayDays(int64_t& today) {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2024) {
        return false;  // La hora aún no se ha sincronizado
    }
    today = DaysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return true;
}

bool StudyScreens::ParseDate(const std::string& text, int& y, int& m, int& d) {
    if (sscanf(text.c_str(), "%d-%d-%d", &y, &m, &d) != 3) {
        if (sscanf(text.c_str(), "%d/%d/%d", &d, &m, &y) != 3) {
            return false;
        }
    }
    return y >= 2024 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

std::vector<StudyScreens::Exam> StudyScreens::LoadExams() {
    Settings settings("study", false);
    std::string raw = settings.GetString("exams", "");
    std::vector<Exam> exams;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) {
            end = raw.size();
        }
        std::string line = raw.substr(start, end - start);
        size_t bar = line.find('|');
        Exam exam;
        if (bar != std::string::npos && ParseDate(line.substr(0, bar), exam.year, exam.month, exam.day)) {
            exam.subject = line.substr(bar + 1);
            exams.push_back(exam);
        }
        start = end + 1;
    }
    return exams;
}

void StudyScreens::SaveExams(const std::vector<Exam>& exams) {
    std::string raw;
    char date[16];
    for (const auto& exam : exams) {
        snprintf(date, sizeof(date), "%04d-%02d-%02d", exam.year, exam.month, exam.day);
        raw += date;
        raw += '|';
        raw += exam.subject;
        raw += '\n';
    }
    Settings settings("study", true);
    settings.SetString("exams", raw);
}

// ---------------------------------------------------------------------------
// Construcción de la interfaz (siempre con la pantalla bloqueada)
// ---------------------------------------------------------------------------

void StudyScreens::BuildPomodoroUi() {
    if (pomo_root_ != nullptr) {
        return;
    }
    pomo_root_ = CreateRoot();

    pomo_arc_ = lv_arc_create(pomo_root_);
    lv_obj_remove_style_all(pomo_arc_);
    lv_obj_set_size(pomo_arc_, 224, 224);
    lv_obj_center(pomo_arc_);
    lv_arc_set_rotation(pomo_arc_, 270);
    lv_arc_set_bg_angles(pomo_arc_, 0, 360);
    lv_arc_set_range(pomo_arc_, 0, 1000);
    lv_arc_set_value(pomo_arc_, 1000);
    lv_obj_remove_flag(pomo_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(pomo_arc_, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(pomo_arc_, ColorOff(), LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomo_arc_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pomo_arc_, ColorStudy(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(pomo_arc_, true, LV_PART_INDICATOR);

    pomo_phase_label_ = CreateLabel(pomo_root_, 50, 170, ColorStudy());
    lv_obj_set_style_text_letter_space(pomo_phase_label_, 1, 0);

    pomo_time_label_ = CreateLabel(pomo_root_, 74, 200, ColorText(), &font_mono_56);
    lv_obj_set_style_text_letter_space(pomo_time_label_, -2, 0);

    pomo_subject_label_ = CreateLabel(pomo_root_, 146, 160, lv_color_hex(0xB0BEC5), &font_mono_20);

    for (int i = 0; i < 4; ++i) {
        pomo_dots_[i] = CreateBox(pomo_root_, 88 + i * 18, 182, 10, 10, LV_RADIUS_CIRCLE, ColorOff());
        lv_obj_set_style_border_width(pomo_dots_[i], 2, 0);
        lv_obj_set_style_border_color(pomo_dots_[i], ColorOff(), 0);
    }
}

void StudyScreens::BuildExamUi() {
    if (exam_root_ != nullptr) {
        return;
    }
    exam_root_ = CreateRoot();
    kit::Corners(exam_root_);
    exam_top_label_ = CreateLabel(exam_root_, 22, 210, lv_color_hex(kit::kHudDim));
    lv_obj_set_style_text_letter_space(exam_top_label_, 1, 0);
    exam_subject_label_ = CreateLabel(exam_root_, 44, 216, ColorText(), &font_mono_20);
    lv_label_set_long_mode(exam_subject_label_, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    exam_days_label_ = CreateLabel(exam_root_, 72, 220, ColorExam(), &font_mono_80);
    exam_unit_label_ = CreateLabel(exam_root_, 164, 200, ColorExam());
    lv_obj_set_style_text_letter_space(exam_unit_label_, 2, 0);
    kit::HLine(exam_root_, 40, 190, 160);
    exam_date_label_ = CreateLabel(exam_root_, 200, 200, lv_color_hex(kit::kHudDim));
}

// ---------------------------------------------------------------------------
// Dibujo (llamar con la pantalla bloqueada y mutex_ tomado)
// ---------------------------------------------------------------------------

void StudyScreens::RenderLocked() {
    const bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    const int64_t now = esp_timer_get_time();

    const bool exam_active = exam_root_ != nullptr && now < exam_until_us_;
    SetVisible(exam_root_, exam_active);

    const bool pomo_visible = pomo_root_ != nullptr && phase_ != Phase::kOff && idle && !exam_active;
    SetVisible(pomo_root_, pomo_visible, false);  // no tapar la pantalla del tiempo
    if (!pomo_visible) {
        return;
    }

    int64_t remaining = paused_ ? paused_remaining_us_ : std::max<int64_t>(0, phase_end_us_ - now);
    int secs = static_cast<int>((remaining + kSecondUs - 1) / kSecondUs);
    int mm = std::min(99, secs / 60);
    int ss = secs % 60;

    lv_color_t color = paused_ ? ColorPause() : (phase_ == Phase::kStudy ? ColorStudy() : ColorBreak());
    int value = phase_total_us_ > 0 ? static_cast<int>(remaining * 1000 / phase_total_us_) : 0;
    lv_arc_set_value(pomo_arc_, value);
    lv_obj_set_style_arc_color(pomo_arc_, color, LV_PART_INDICATOR);

    // Los dos puntos parpadean mientras corre el tiempo
    char time_text[8];
    const bool colon = paused_ || (secs % 2 == 0);
    snprintf(time_text, sizeof(time_text), colon ? "%02d:%02d" : "%02d %02d", mm, ss);
    lv_label_set_text(pomo_time_label_, time_text);
    lv_obj_set_style_text_color(pomo_time_label_, paused_ ? ColorPause() : ColorText(), 0);

    const char* phase_text = "\u25CF ESTUDIO";
    if (paused_) {
        phase_text = "\u25CF PAUSA";
    } else if (phase_ == Phase::kBreak) {
        phase_text = (completed_ > 0 && completed_ % 4 == 0) ? "\u25CF DESCANSO LARGO" : "\u25CF DESCANSO";
    }
    lv_label_set_text(pomo_phase_label_, phase_text);
    lv_obj_set_style_text_color(pomo_phase_label_, color, 0);

    std::string subject_line = phase_ == Phase::kStudy ? subject_ : "Levántate y estira";
    lv_label_set_text(pomo_subject_label_, subject_line.c_str());

    int filled = completed_ % 4;
    if (phase_ == Phase::kBreak && completed_ > 0 && completed_ % 4 == 0) {
        filled = 4;
    }
    for (int i = 0; i < 4; ++i) {
        bool done = i < filled;
        bool current = !done && i == filled && phase_ == Phase::kStudy;
        lv_obj_set_style_bg_color(pomo_dots_[i], done ? color : ColorBlack(), 0);
        lv_obj_set_style_border_color(pomo_dots_[i], (done || current) ? color : ColorOff(), 0);
    }
}

// ---------------------------------------------------------------------------
// Lógica del Pomodoro
// ---------------------------------------------------------------------------

void StudyScreens::EnsureTimer() {
    if (tick_timer_ != nullptr) {
        return;
    }
    esp_timer_create_args_t args = {};
    args.callback = &StudyScreens::TickCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "study_tick";
    args.skip_unhandled_events = true;
    ESP_ERROR_CHECK(esp_timer_create(&args, &tick_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer_, kSecondUs));
}

void StudyScreens::TickCallback(void* arg) {
    static_cast<StudyScreens*>(arg)->Tick();
}

void StudyScreens::Tick() {
    bool play_success = false;
    bool play_popup = false;
    bool active = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        const int64_t now = esp_timer_get_time();
        if (phase_ != Phase::kOff && !paused_ && now >= phase_end_us_) {
            if (phase_ == Phase::kStudy) {
                completed_++;
                const bool long_break = completed_ % 4 == 0;
                phase_ = Phase::kBreak;
                phase_total_us_ = (long_break ? 15 : break_minutes_) * kMinuteUs;
                play_success = true;
            } else {
                phase_ = Phase::kStudy;
                round_++;
                phase_total_us_ = study_minutes_ * kMinuteUs;
                play_popup = true;
            }
            phase_end_us_ = now + phase_total_us_;
        }
        active = phase_ != Phase::kOff || now < exam_until_us_ + 2 * kSecondUs;
    }
    if (!active) {
        return;
    }
    // Dibujamos y sonamos desde la tarea principal, no desde el temporizador
    auto& app = Application::GetInstance();
    app.Schedule([this, play_success, play_popup]() {
        auto& app = Application::GetInstance();
        if (play_success) {
            app.PlaySound(Lang::Sounds::OGG_SUCCESS);
        } else if (play_popup) {
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        DisplayLockGuard lock(display_);
        std::lock_guard<std::mutex> guard(mutex_);
        RenderLocked();
    });
}

void StudyScreens::StartPomodoro(int study_min, int break_min, const std::string& subject) {
    {
        DisplayLockGuard lock(display_);
        BuildPomodoroUi();
    }
    {
        std::lock_guard<std::mutex> guard(mutex_);
        study_minutes_ = study_min;
        break_minutes_ = break_min;
        subject_ = ToScreenText(subject.empty() ? std::string("Estudio") : subject, 18);
        phase_ = Phase::kStudy;
        paused_ = false;
        round_ = 1;
        completed_ = 0;
        phase_total_us_ = study_min * kMinuteUs;
        phase_end_us_ = esp_timer_get_time() + phase_total_us_;
    }
    EnsureTimer();
    DisplayLockGuard lock(display_);
    std::lock_guard<std::mutex> guard(mutex_);
    RenderLocked();
}

void StudyScreens::StopPomodoro() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        phase_ = Phase::kOff;
        paused_ = false;
    }
    DisplayLockGuard lock(display_);
    std::lock_guard<std::mutex> guard(mutex_);
    RenderLocked();
}

bool StudyScreens::TogglePause() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (phase_ == Phase::kOff) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (paused_) {
        phase_end_us_ = now + paused_remaining_us_;
        paused_ = false;
    } else {
        paused_remaining_us_ = std::max<int64_t>(0, phase_end_us_ - now);
        paused_ = true;
    }
    return true;
}

std::string StudyScreens::PomodoroStatus() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (phase_ == Phase::kOff) {
        return "No hay ningún Pomodoro en marcha.";
    }
    const int64_t now = esp_timer_get_time();
    int64_t remaining = paused_ ? paused_remaining_us_ : std::max<int64_t>(0, phase_end_us_ - now);
    int secs = static_cast<int>(remaining / kSecondUs);
    char buf[200];
    snprintf(buf, sizeof(buf), "%s%s, ronda %d, quedan %d min %d s. Rondas completadas: %d.",
             phase_ == Phase::kStudy ? "Estudiando" : "En descanso", paused_ ? " (en pausa)" : "",
             round_, secs / 60, secs % 60, completed_);
    return buf;
}

// ---------------------------------------------------------------------------
// Exámenes
// ---------------------------------------------------------------------------

bool StudyScreens::ShowNextExam(std::string& message) {
    int64_t today = 0;
    if (!TodayDays(today)) {
        message = "Todavía no sé la fecha de hoy; inténtalo en un momento.";
        return false;
    }
    std::vector<Exam> exams = LoadExams();
    const Exam* next = nullptr;
    int64_t best = INT64_MAX;
    for (const auto& exam : exams) {
        int64_t days = DaysFromCivil(exam.year, exam.month, exam.day) - today;
        if (days >= 0 && days < best) {
            best = days;
            next = &exam;
        }
    }
    if (next == nullptr) {
        message = "No hay exámenes próximos guardados.";
        return false;
    }

    {
        DisplayLockGuard lock(display_);
        BuildExamUi();
        const int days = static_cast<int>(std::min<int64_t>(best, 999));
        char days_text[8];
        snprintf(days_text, sizeof(days_text), "%d", days);
        lv_label_set_text(exam_days_label_, days_text);
        // Rojo si queda una semana o menos, ámbar si queda un mes, lila si hay tiempo
        lv_color_t urgency = lv_color_hex(days <= 7 ? 0xFF6E6E : (days <= 30 ? 0xFFB74D : 0xC3A6FF));
        lv_obj_set_style_text_color(exam_days_label_, urgency, 0);
        lv_obj_set_style_text_color(exam_unit_label_, urgency, 0);
        lv_label_set_text(exam_top_label_, "\u25B8 PR\u00D3XIMO EXAMEN");
        lv_label_set_text(exam_unit_label_, days == 0 ? "\u00A1ES HOY!" : (days == 1 ? "D\u00CDA" : "D\u00CDAS"));
        std::string subject = ToScreenText(next->subject, 22);
        lv_label_set_text(exam_subject_label_, subject.c_str());
        char date[24];
        snprintf(date, sizeof(date), "%d %s %d", next->day, kMonths[next->month - 1], next->year);
        lv_label_set_text(exam_date_label_, date);

        std::lock_guard<std::mutex> guard(mutex_);
        exam_until_us_ = esp_timer_get_time() + kExamScreenUs;
        RenderLocked();
    }
    EnsureTimer();

    char buf[200];
    if (best == 0) {
        snprintf(buf, sizeof(buf), "El examen de %s es hoy.", next->subject.c_str());
    } else {
        snprintf(buf, sizeof(buf), "Quedan %d días para el examen de %s (%04d-%02d-%02d).",
                 static_cast<int>(best), next->subject.c_str(), next->year, next->month, next->day);
    }
    message = buf;
    return true;
}

// ---------------------------------------------------------------------------
// Herramientas que puede usar la IA
// ---------------------------------------------------------------------------

void StudyScreens::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.pomodoro.start",
                "Inicia un Pomodoro en la pantalla del robot (anillo con cuenta atrás). Úsalo cuando el "
                "usuario quiera empezar a estudiar o pida un temporizador de estudio. Start a study "
                "Pomodoro timer on the robot screen.",
                PropertyList({
                    Property("minutes", kPropertyTypeInteger, 25, 1, 120),
                    Property("break_minutes", kPropertyTypeInteger, 5, 1, 60),
                    Property("subject", kPropertyTypeString, std::string("Estudio")),
                }),
                [this](const PropertyList& p) -> ToolResult {
                    int minutes = p["minutes"].value<int>();
                    int break_minutes = p["break_minutes"].value<int>();
                    std::string subject = p["subject"].value<std::string>();
                    StartPomodoro(minutes, break_minutes, subject);
                    char buf[160];
                    snprintf(buf, sizeof(buf), "Pomodoro iniciado: %d min de %s y %d min de descanso.",
                             minutes, subject.c_str(), break_minutes);
                    return std::string(buf);
                });

    mcp.AddTool("self.pomodoro.stop",
                "Termina y quita el Pomodoro de la pantalla. Stop the Pomodoro timer.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    StopPomodoro();
                    return std::string("Pomodoro terminado.");
                });

    mcp.AddTool("self.pomodoro.pause_resume",
                "Pausa el Pomodoro, o lo reanuda si ya estaba en pausa. Pause or resume the Pomodoro.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    if (!TogglePause()) {
                        return std::unexpected(std::string("No hay ningún Pomodoro en marcha."));
                    }
                    return PomodoroStatus();
                });

    mcp.AddTool("self.pomodoro.status",
                "Dice cuánto queda del Pomodoro actual y cuántas rondas lleva. Get Pomodoro status.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult { return PomodoroStatus(); });

    mcp.AddTool("self.exams.add",
                "Guarda un examen para la cuenta atrás. La fecha debe ir en formato AAAA-MM-DD (convierte "
                "tú fechas como 'el 9 de diciembre' usando la fecha actual). Save an exam date.",
                PropertyList({
                    Property("subject", kPropertyTypeString),
                    Property("date", kPropertyTypeString),
                }),
                [this](const PropertyList& p) -> ToolResult {
                    Exam exam;
                    exam.subject = p["subject"].value<std::string>();
                    if (exam.subject.empty() || exam.subject.size() > 60) {
                        return std::unexpected(std::string("Nombre de asignatura no válido."));
                    }
                    if (!ParseDate(p["date"].value<std::string>(), exam.year, exam.month, exam.day)) {
                        return std::unexpected(std::string("Fecha no válida. Usa el formato AAAA-MM-DD."));
                    }
                    std::vector<Exam> exams = LoadExams();
                    exams.erase(std::remove_if(exams.begin(), exams.end(),
                                               [&](const Exam& e) { return e.subject == exam.subject; }),
                                exams.end());
                    if (exams.size() >= kMaxExams) {
                        exams.erase(exams.begin());
                    }
                    exams.push_back(exam);
                    SaveExams(exams);
                    std::string message;
                    ShowNextExam(message);
                    return std::string("Examen guardado. ") + message;
                });

    mcp.AddTool("self.exams.list",
                "Lista los exámenes guardados con los días que faltan. List saved exams.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    std::vector<Exam> exams = LoadExams();
                    if (exams.empty()) {
                        return std::string("No hay exámenes guardados.");
                    }
                    int64_t today = 0;
                    bool has_today = TodayDays(today);
                    std::string out;
                    char line[160];
                    for (const auto& exam : exams) {
                        int64_t days = has_today ? DaysFromCivil(exam.year, exam.month, exam.day) - today : -1;
                        if (has_today && days < 0) {
                            continue;
                        }
                        if (has_today) {
                            snprintf(line, sizeof(line), "%s: %04d-%02d-%02d (faltan %d días). ",
                                     exam.subject.c_str(), exam.year, exam.month, exam.day, static_cast<int>(days));
                        } else {
                            snprintf(line, sizeof(line), "%s: %04d-%02d-%02d. ", exam.subject.c_str(),
                                     exam.year, exam.month, exam.day);
                        }
                        out += line;
                    }
                    return out.empty() ? std::string("No hay exámenes próximos.") : out;
                });

    mcp.AddTool("self.exams.remove",
                "Borra un examen guardado por el nombre de la asignatura. Remove a saved exam.",
                PropertyList({Property("subject", kPropertyTypeString)}),
                [this](const PropertyList& p) -> ToolResult {
                    std::string subject = p["subject"].value<std::string>();
                    std::vector<Exam> exams = LoadExams();
                    size_t before = exams.size();
                    exams.erase(std::remove_if(exams.begin(), exams.end(),
                                               [&](const Exam& e) { return e.subject == subject; }),
                                exams.end());
                    if (exams.size() == before) {
                        return std::unexpected(std::string("No encuentro ese examen."));
                    }
                    SaveExams(exams);
                    return std::string("Examen borrado.");
                });

    mcp.AddTool("self.exams.show_next",
                "Muestra en la pantalla la cuenta atrás del próximo examen durante unos segundos. "
                "Show the next exam countdown on screen.",
                PropertyList(),
                [this](const PropertyList&) -> ToolResult {
                    std::string message;
                    ShowNextExam(message);
                    return message;
                });

    ESP_LOGI(TAG, "Herramientas de estudio registradas");
}
