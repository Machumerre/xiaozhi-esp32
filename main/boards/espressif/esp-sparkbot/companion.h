// "Alma" del robot: racha de estudio, experiencia y nivel, memoria de lo que le cuentas.
#ifndef COMPANION_H
#define COMPANION_H

#include <ctime>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Companion {
public:
    static Companion& Get();

    struct Snapshot {
        int level = 1;
        int xp = 0;
        int level_start_xp = 0;
        int next_level_xp = 50;
        int streak = 0;
        int today_minutes = 0;
        bool studied_today = false;
    };

    // Llamar desde cualquier tarea
    void AddStudyMinutes(int minutes);
    void AddConversation();
    void AddXp(int amount);
    Snapshot GetSnapshot();
    int TakeLevelUp();  // devuelve el nivel nuevo si acaba de subir, si no 0
    bool ShouldRemindStreak(const struct tm& local);

    void RegisterTools();

private:
    Companion();

    std::mutex mutex_;
    bool loaded_ = false;
    int xp_ = 0;
    int streak_ = 0;
    int best_streak_ = 0;
    int64_t last_study_day_ = -100;
    int64_t today_day_ = -1;
    int today_minutes_ = 0;
    int today_conversation_xp_ = 0;
    int total_minutes_ = 0;
    int week_minutes_[7] = {};       // por día (índice = día % 7)
    int64_t week_days_[7] = {};
    int pending_level_up_ = 0;
    int64_t reminded_day_ = -1;
    std::vector<std::string> facts_;

    void LoadLocked();
    void SaveLocked();
    void SaveFactsLocked();
    void RollDayLocked(int64_t today);
    void AddXpLocked(int amount);
    static int LevelFor(int xp);
    static int LevelStart(int level);
    static bool Today(int64_t& days);
    std::string PersonalityLocked();
    std::string StatsLocked();
};

#endif  // COMPANION_H
