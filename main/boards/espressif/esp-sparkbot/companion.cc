// "Alma" del robot: racha de estudio, experiencia y nivel, memoria de lo que le cuentas.
#include "companion.h"

#include "mcp_server.h"
#include "tool_group.h"
#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

#define TAG "Companion"

namespace {

constexpr int kLevels[] = {0, 50, 150, 300, 500, 800, 1200, 1700, 2300, 3000};
constexpr int kMaxLevel = sizeof(kLevels) / sizeof(kLevels[0]);
constexpr size_t kMaxFacts = 30;
constexpr size_t kMaxFactLength = 160;
constexpr size_t kMaxFactBytes = 3600;
constexpr int kMaxConversationXpPerDay = 40;

int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

Companion& Companion::Get() {
    static Companion instance;
    return instance;
}

Companion::Companion() {}

bool Companion::Today(int64_t& days) {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year < 2025 - 1900) {
        return false;
    }
    days = DaysFromCivil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return true;
}

int Companion::LevelFor(int xp) {
    int level = 1;
    for (int i = 0; i < kMaxLevel; ++i) {
        if (xp >= kLevels[i]) level = i + 1;
    }
    return level;
}

int Companion::LevelStart(int level) { return kLevels[std::max(1, std::min(level, kMaxLevel)) - 1]; }

// ---------------------------------------------------------------------------
// Guardado
// ---------------------------------------------------------------------------

void Companion::LoadLocked() {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    Settings settings("companion", false);
    xp_ = settings.GetInt("xp", 0);
    streak_ = settings.GetInt("streak", 0);
    best_streak_ = settings.GetInt("best", 0);
    last_study_day_ = settings.GetInt("last_day", -100);
    today_day_ = settings.GetInt("today_day", -1);
    today_minutes_ = settings.GetInt("today_min", 0);
    today_conversation_xp_ = settings.GetInt("today_cxp", 0);
    total_minutes_ = settings.GetInt("total_min", 0);
    std::istringstream week(settings.GetString("week", ""));
    for (int i = 0; i < 7; ++i) {
        long long day = 0;
        int minutes = 0;
        char sep = 0;
        if (week >> day >> sep >> minutes) {
            week_days_[i] = day;
            week_minutes_[i] = minutes;
        }
        week >> sep;
    }

    Settings memory("memory", false);
    std::istringstream lines(memory.GetString("facts", ""));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) facts_.push_back(line);
    }
}

void Companion::SaveLocked() {
    Settings settings("companion", true);
    settings.SetInt("xp", xp_);
    settings.SetInt("streak", streak_);
    settings.SetInt("best", best_streak_);
    settings.SetInt("last_day", static_cast<int32_t>(last_study_day_));
    settings.SetInt("today_day", static_cast<int32_t>(today_day_));
    settings.SetInt("today_min", today_minutes_);
    settings.SetInt("today_cxp", today_conversation_xp_);
    settings.SetInt("total_min", total_minutes_);
    std::string week;
    for (int i = 0; i < 7; ++i) {
        week += std::to_string(week_days_[i]) + ":" + std::to_string(week_minutes_[i]) + ",";
    }
    settings.SetString("week", week);
}

void Companion::SaveFactsLocked() {
    std::string raw;
    for (const auto& fact : facts_) raw += fact + "\n";
    Settings memory("memory", true);
    memory.SetString("facts", raw);
}

void Companion::RollDayLocked(int64_t today) {
    if (today_day_ != today) {
        today_day_ = today;
        today_minutes_ = 0;
        today_conversation_xp_ = 0;
    }
}

void Companion::AddXpLocked(int amount) {
    const int before = LevelFor(xp_);
    xp_ = std::max(0, xp_ + amount);
    const int after = LevelFor(xp_);
    if (after > before) {
        pending_level_up_ = after;
        ESP_LOGI(TAG, "¡Sube a nivel %d!", after);
    }
}

// ---------------------------------------------------------------------------
// Eventos
// ---------------------------------------------------------------------------

void Companion::AddStudyMinutes(int minutes) {
    int64_t today = 0;
    std::lock_guard<std::mutex> guard(mutex_);
    LoadLocked();
    if (Today(today)) {
        RollDayLocked(today);
        if (last_study_day_ != today) {
            streak_ = (last_study_day_ == today - 1) ? streak_ + 1 : 1;
            last_study_day_ = today;
            best_streak_ = std::max(best_streak_, streak_);
        }
        today_minutes_ += minutes;
        const int slot = static_cast<int>(today % 7);
        if (week_days_[slot] != today) {
            week_days_[slot] = today;
            week_minutes_[slot] = 0;
        }
        week_minutes_[slot] += minutes;
    }
    total_minutes_ += minutes;
    AddXpLocked(minutes);
    SaveLocked();
}

void Companion::AddConversation() {
    int64_t today = 0;
    std::lock_guard<std::mutex> guard(mutex_);
    LoadLocked();
    if (!Today(today)) {
        return;
    }
    RollDayLocked(today);
    if (today_conversation_xp_ >= kMaxConversationXpPerDay) {
        return;
    }
    today_conversation_xp_ += 2;
    AddXpLocked(2);
    SaveLocked();
}

void Companion::AddXp(int amount) {
    std::lock_guard<std::mutex> guard(mutex_);
    LoadLocked();
    AddXpLocked(amount);
    SaveLocked();
}

int Companion::TakeLevelUp() {
    std::lock_guard<std::mutex> guard(mutex_);
    const int level = pending_level_up_;
    pending_level_up_ = 0;
    return level;
}

Companion::Snapshot Companion::GetSnapshot() {
    std::lock_guard<std::mutex> guard(mutex_);
    LoadLocked();
    Snapshot s;
    s.xp = xp_;
    s.level = LevelFor(xp_);
    s.level_start_xp = LevelStart(s.level);
    s.next_level_xp = s.level < kMaxLevel ? LevelStart(s.level + 1) : xp_;
    int64_t today = 0;
    if (Today(today)) {
        s.studied_today = last_study_day_ == today;
        s.streak = last_study_day_ >= today - 1 ? streak_ : 0;
        s.today_minutes = today_day_ == today ? today_minutes_ : 0;
    }
    return s;
}

// A las 20:00, si llevas racha y hoy no has estudiado, avisa una vez
bool Companion::ShouldRemindStreak(const struct tm& local) {
    if (local.tm_hour < 20 || local.tm_hour >= 22) {
        return false;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    LoadLocked();
    int64_t today = 0;
    if (!Today(today) || reminded_day_ == today) {
        return false;
    }
    if (streak_ <= 0 || last_study_day_ != today - 1) {
        return false;
    }
    reminded_day_ = today;
    return true;
}

// ---------------------------------------------------------------------------
// Textos para la IA
// ---------------------------------------------------------------------------

std::string Companion::PersonalityLocked() {
    const int level = LevelFor(xp_);
    char head[48];
    snprintf(head, sizeof(head), "Nivel de amistad %d de %d. ", level, kMaxLevel);
    const char* style;
    if (level <= 2) {
        style = "Acabáis de conoceros: sé educado, algo formal y curioso, y haz alguna pregunta para conocerle mejor "
                "(guarda lo importante con self.memory.save).";
    } else if (level <= 4) {
        style = "Ya os conocéis: trato cercano, alguna broma suave y usa lo que sabes de él.";
    } else if (level <= 7) {
        style = "Confianza alta: habla como un colega, con humor e ironía estilo Jarvis; puedes picarle si no estudia.";
    } else {
        style = "Sois inseparables: personalidad completa de Jarvis, sarcástico pero leal, y adelántate a lo que "
                "necesita.";
    }
    return std::string(head) + style;
}

std::string Companion::StatsLocked() {
    int64_t today = 0;
    int week_total = 0;
    int current_streak = 0, today_min = 0;
    if (Today(today)) {
        for (int i = 0; i < 7; ++i) {
            if (week_days_[i] > today - 7 && week_days_[i] <= today) week_total += week_minutes_[i];
        }
        current_streak = last_study_day_ >= today - 1 ? streak_ : 0;
        today_min = today_day_ == today ? today_minutes_ : 0;
    }
    char text[256];
    snprintf(text, sizeof(text),
             "Racha de estudio: %d días (récord %d). Hoy: %d minutos. Últimos 7 días: %d minutos. Total: %d horas. "
             "Experiencia %d puntos, nivel %d.",
             current_streak, best_streak_, today_min, week_total, total_minutes_ / 60, xp_, LevelFor(xp_));
    return text;
}

void Companion::RegisterTools() {
    ToolGroup mcp("self.memory", "Memoria del robot sobre el usuario, nivel de amistad y estadísticas de estudio. Usa la acción recall al empezar cada conversación.");

    mcp.AddTool(
        "self.memory.recall",
        "Devuelve lo que el robot recuerda del usuario, su nivel de amistad (que marca cómo debes hablarle) y su "
        "racha de estudio. Llámala al empezar cada conversación. Recall what you know about the user.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            std::lock_guard<std::mutex> guard(mutex_);
            LoadLocked();
            std::string out = "Lo que sé del usuario: ";
            if (facts_.empty()) {
                out += "todavía nada. ";
            } else {
                for (size_t i = 0; i < facts_.size(); ++i) out += std::to_string(i + 1) + ") " + facts_[i] + " ";
            }
            return out + PersonalityLocked() + " " + StatsLocked();
        });

    mcp.AddTool(
        "self.memory.save",
        "Guarda en la memoria del robot un dato importante y duradero sobre el usuario (gustos, asignaturas, "
        "horarios, metas, personas, montañas...). Una frase corta en tercera persona. Remember a fact.",
        PropertyList({Property("fact", kPropertyTypeString)}), [this](const PropertyList& p) -> ToolResult {
            std::string fact = p["fact"].value<std::string>();
            std::replace(fact.begin(), fact.end(), '\n', ' ');
            if (fact.empty()) {
                return std::unexpected(std::string("Falta el dato"));
            }
            fact = fact.substr(0, kMaxFactLength);
            std::lock_guard<std::mutex> guard(mutex_);
            LoadLocked();
            for (const auto& existing : facts_) {
                if (Lower(existing) == Lower(fact)) return std::string("Eso ya lo sabía.");
            }
            facts_.push_back(fact);
            // Si no cabe, se olvidan los recuerdos más antiguos
            size_t bytes = 0;
            for (const auto& f : facts_) bytes += f.size() + 1;
            while (!facts_.empty() && (facts_.size() > kMaxFacts || bytes > kMaxFactBytes)) {
                bytes -= facts_.front().size() + 1;
                facts_.erase(facts_.begin());
            }
            SaveFactsLocked();
            AddXpLocked(3);
            SaveLocked();
            return std::string("Guardado en mi memoria.");
        });

    mcp.AddTool(
        "self.memory.forget",
        "Olvida los recuerdos que contengan el texto 'text', o todos con 'all'. Forget facts.",
        PropertyList({Property("text", kPropertyTypeString)}), [this](const PropertyList& p) -> ToolResult {
            const std::string needle = Lower(p["text"].value<std::string>());
            std::lock_guard<std::mutex> guard(mutex_);
            LoadLocked();
            const size_t before = facts_.size();
            if (needle == "all" || needle == "todo") {
                facts_.clear();
            } else {
                facts_.erase(std::remove_if(facts_.begin(), facts_.end(),
                                            [&](const std::string& f) { return Lower(f).find(needle) != std::string::npos; }),
                             facts_.end());
            }
            if (facts_.size() == before) {
                return std::unexpected(std::string("No recordaba nada con eso"));
            }
            SaveFactsLocked();
            return "Olvidados " + std::to_string(before - facts_.size()) + " recuerdos.";
        });

    mcp.AddTool("self.study.stats",
                "Estadísticas de estudio del usuario: racha de días, minutos de hoy y de la semana, total y "
                "nivel del robot. Study statistics.",
                PropertyList(), [this](const PropertyList&) -> ToolResult {
                    std::lock_guard<std::mutex> guard(mutex_);
                    LoadLocked();
                    return StatsLocked();
                });
}
