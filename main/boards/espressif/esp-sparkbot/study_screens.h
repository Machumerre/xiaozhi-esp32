// Pantallas propias para el ESP-SparkBot: Pomodoro y cuenta atrás de exámenes.
// Se controlan por voz a través de herramientas MCP (la IA las llama).
#ifndef STUDY_SCREENS_H
#define STUDY_SCREENS_H

#include <esp_timer.h>
#include <lvgl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Display;

class StudyScreens {
public:
    explicit StudyScreens(Display* display);
    void RegisterTools();

private:
    enum class Phase { kOff, kStudy, kBreak };

    struct SevenSeg {
        lv_obj_t* seg[7] = {};
    };

    struct Exam {
        std::string subject;
        int year = 0;
        int month = 0;
        int day = 0;
    };

    Display* display_;
    std::mutex mutex_;
    esp_timer_handle_t tick_timer_ = nullptr;

    // Estado del Pomodoro
    Phase phase_ = Phase::kOff;
    bool paused_ = false;
    int64_t phase_end_us_ = 0;
    int64_t paused_remaining_us_ = 0;
    int64_t phase_total_us_ = 0;
    int study_minutes_ = 25;
    int break_minutes_ = 5;
    int round_ = 1;
    int completed_ = 0;
    std::string subject_;

    // Pantalla de exámenes
    int64_t exam_until_us_ = 0;

    // Objetos LVGL (se crean la primera vez que se usan)
    lv_obj_t* pomo_root_ = nullptr;
    lv_obj_t* pomo_arc_ = nullptr;
    lv_obj_t* pomo_phase_label_ = nullptr;
    lv_obj_t* pomo_subject_label_ = nullptr;
    lv_obj_t* pomo_colon_[2] = {};
    lv_obj_t* pomo_dots_[4] = {};
    SevenSeg pomo_digits_[4];

    lv_obj_t* exam_root_ = nullptr;
    lv_obj_t* exam_top_label_ = nullptr;
    lv_obj_t* exam_unit_label_ = nullptr;
    lv_obj_t* exam_subject_label_ = nullptr;
    lv_obj_t* exam_date_label_ = nullptr;
    SevenSeg exam_digits_[3];

    // Lógica
    void StartPomodoro(int study_min, int break_min, const std::string& subject);
    void StopPomodoro();
    bool TogglePause();
    std::string PomodoroStatus();
    void Tick();
    static void TickCallback(void* arg);
    void EnsureTimer();

    // Exámenes
    std::vector<Exam> LoadExams();
    void SaveExams(const std::vector<Exam>& exams);
    static bool ParseDate(const std::string& text, int& y, int& m, int& d);
    static int64_t DaysFromCivil(int y, int m, int d);
    static bool TodayDays(int64_t& today);
    bool ShowNextExam(std::string& message);

    // Dibujo
    void BuildPomodoroUi();
    void BuildExamUi();
    static void CreateSevenSeg(lv_obj_t* parent, SevenSeg& digit, int x, int y, int w, int h, int t);
    static void SetSevenSeg(SevenSeg& digit, int value, lv_color_t on, lv_color_t off);
    void RenderLocked();
    static std::string ToScreenText(const std::string& utf8, size_t max_chars);
};

#endif  // STUDY_SCREENS_H
