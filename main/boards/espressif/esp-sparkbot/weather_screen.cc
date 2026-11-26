// Pantalla del tiempo con iconos animados para el ESP-SparkBot (datos de Open-Meteo).
#include "weather_screen.h"

#include "board.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"
#include "web_search_format.h"

#include <cJSON.h>
#include <esp_log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#define TAG "Weather"

namespace {

constexpr uint32_t kShowMs = 25000;
constexpr size_t kMaxResponse = 24 * 1024;

constexpr uint32_t kSun = 0xFFC93C;
constexpr uint32_t kRay = 0xFFB300;
constexpr uint32_t kCloudWhite = 0xECEFF1;
constexpr uint32_t kCloudGray = 0x90A4AE;
constexpr uint32_t kCloudDark = 0x607D8B;
constexpr uint32_t kRain = 0x4FC3F7;
constexpr uint32_t kSnow = 0xFFFFFF;
constexpr uint32_t kMoon = 0xFFF3C4;

struct JsonDeleter {
    void operator()(cJSON* json) const { cJSON_Delete(json); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

double NumberAt(const cJSON* array, int index, double fallback = 0) {
    const cJSON* item = cJSON_GetArrayItem(array, index);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

int Round(double value) { return static_cast<int>(std::lround(value)); }

int64_t DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

const char* DayName(const char* iso_date, bool spoken) {
    static const char* kScreen[7] = {"DOMINGO", "LUNES", "MARTES", "MIÉRCOLES",
                                     "JUEVES", "VIERNES", "SÁBADO"};
    static const char* kSpoken[7] = {"domingo", "lunes", "martes", "miércoles",
                                     "jueves", "viernes", "sábado"};
    int y = 0, m = 0, d = 0;
    if (iso_date == nullptr || sscanf(iso_date, "%d-%d-%d", &y, &m, &d) != 3) {
        return "";
    }
    int64_t days = DaysFromCivil(y, m, d);
    int weekday = static_cast<int>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 fue jueves
    return spoken ? kSpoken[weekday] : kScreen[weekday];
}

// --- Piezas de los iconos animados --------------------------------------------------

void Sun(lv_obj_t* parent, int cx, int cy, int size) {
    lv_obj_t* group = kit::Group(parent, cx - size / 2, cy - size / 2, size, size);
    lv_obj_set_style_transform_pivot_x(group, size / 2, 0);
    lv_obj_set_style_transform_pivot_y(group, size / 2, 0);
    const int dot = std::max(4, size * 14 / 100);
    const double radius = size * 0.42;
    for (int k = 0; k < 8; ++k) {
        double angle = k * M_PI / 4.0;
        int x = static_cast<int>(size / 2 + radius * std::cos(angle)) - dot / 2;
        int y = static_cast<int>(size / 2 + radius * std::sin(angle)) - dot / 2;
        kit::Box(group, x, y, dot, dot, LV_RADIUS_CIRCLE, kRay);
    }
    const int core = size * 52 / 100;
    kit::Box(group, (size - core) / 2, (size - core) / 2, core, core, LV_RADIUS_CIRCLE, kSun);
    kit::Loop(group, kit::SetRotation, 0, 3600, 12000);
}

void Moon(lv_obj_t* parent, int cx, int cy, int size) {
    kit::Box(parent, cx - size / 2, cy - size / 2, size, size, LV_RADIUS_CIRCLE, kMoon);
    kit::Box(parent, cx - size / 2 + size * 32 / 100, cy - size / 2 - size * 12 / 100, size, size,
             LV_RADIUS_CIRCLE, 0x000000);
    const int stars[3][2] = {{cx + size / 2 + 6, cy - size / 2}, {cx + size / 2 + 18, cy + 4},
                             {cx - size / 2 - 10, cy + size / 3}};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* star = kit::Box(parent, stars[i][0], stars[i][1], 5, 5, LV_RADIUS_CIRCLE, 0xFFFFFF);
        kit::Loop(star, kit::SetOpacity, 40, 255, 900, i * 350, true);
    }
}

lv_obj_t* Cloud(lv_obj_t* parent, int x, int y, int scale_pct, uint32_t color, uint32_t delay) {
    auto s = [scale_pct](int v) { return v * scale_pct / 100; };
    lv_obj_t* group = kit::Group(parent, x, y, s(74), s(46));
    kit::Box(group, 0, s(18), s(74), s(28), s(14), color);
    kit::Box(group, s(10), s(6), s(30), s(30), LV_RADIUS_CIRCLE, color);
    kit::Box(group, s(30), 0, s(38), s(38), LV_RADIUS_CIRCLE, color);
    kit::Loop(group, kit::SetTranslateX, -5, 5, 2600, delay, true);
    return group;
}

void Drops(lv_obj_t* parent, int count, int x0, int spacing, int y_from, int y_to, uint32_t time,
           uint32_t color, int w, int h) {
    for (int i = 0; i < count; ++i) {
        lv_obj_t* drop = kit::Box(parent, x0 + i * spacing, y_from, w, h, w / 2, color);
        kit::Loop(drop, kit::SetY, y_from, y_to, time, i * time / count);
    }
}

void Bolt(lv_obj_t* parent, int x, int y) {
    lv_obj_t* group = kit::Group(parent, x, y, 26, 40);
    kit::Box(group, 12, 0, 7, 18, 2, 0xFFE34D);
    kit::Box(group, 5, 15, 18, 6, 2, 0xFFE34D);
    kit::Box(group, 5, 19, 7, 20, 2, 0xFFE34D);
    kit::Loop(group, kit::SetOpacity, 0, 255, 120, 0, true, 1400);
}

void Fog(lv_obj_t* parent) {
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* bar = kit::Box(parent, 16, 28 + i * 16, 80, 7, 3, kCloudGray);
        kit::Loop(bar, kit::SetTranslateX, -9, 9, 2000, i * 300, true);
    }
}

}  // namespace

WeatherScreen::WeatherScreen(Display* display) : display_(display) {}

// --- Red ----------------------------------------------------------------------------

std::string WeatherScreen::UrlEncode(const std::string& text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::expected<std::string, std::string> WeatherScreen::HttpGet(const std::string& url) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(3) : nullptr;
    if (!http) {
        return std::unexpected(std::string("Sin conexión a internet"));
    }
    http->SetTimeout(8000);
    if (auto opened = http->Open("GET", url); !opened) {
        return std::unexpected("No se pudo conectar: " + opened.error().ToString());
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return std::unexpected("Respuesta no válida: " + status.error().ToString());
    }
    if (*status != 200) {
        http->Close();
        return std::unexpected("El servicio del tiempo respondió " + std::to_string(*status));
    }
    auto body = ReadBounded(*http, kMaxResponse);
    http->Close();
    return body;
}

std::expected<WeatherScreen::Place, std::string> WeatherScreen::Geocode(const std::string& name) {
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=es&format=json&name=" +
                      UrlEncode(name);
    auto body = HttpGet(url);
    if (!body) {
        return std::unexpected(body.error());
    }
    JsonPtr json(cJSON_Parse(body->c_str()));
    const cJSON* results = json ? cJSON_GetObjectItem(json.get(), "results") : nullptr;
    const cJSON* first = cJSON_GetArrayItem(results, 0);
    if (first == nullptr) {
        return std::unexpected("No encuentro el lugar \"" + name + "\"");
    }
    Place place;
    const cJSON* item = cJSON_GetObjectItem(first, "name");
    place.name = cJSON_IsString(item) ? item->valuestring : name;
    item = cJSON_GetObjectItem(first, "latitude");
    place.latitude = cJSON_IsNumber(item) ? item->valuedouble : 0;
    item = cJSON_GetObjectItem(first, "longitude");
    place.longitude = cJSON_IsNumber(item) ? item->valuedouble : 0;
    item = cJSON_GetObjectItem(first, "elevation");
    if (cJSON_IsNumber(item)) {
        place.has_elevation = true;
        place.elevation = Round(item->valuedouble);
    }
    return place;
}

std::expected<WeatherScreen::Report, std::string> WeatherScreen::Fetch(const Place& place, int days_ahead) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,wind_speed_10m,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
             "precipitation_probability_max,wind_speed_10m_max&timezone=auto&forecast_days=7",
             place.latitude, place.longitude);
    std::string full_url = url;
    if (place.has_elevation) {
        full_url += "&elevation=" + std::to_string(place.elevation);
    }
    auto body = HttpGet(full_url);
    if (!body) {
        return std::unexpected(body.error());
    }
    JsonPtr json(cJSON_Parse(body->c_str()));
    if (!json) {
        return std::unexpected(std::string("Datos del tiempo no válidos"));
    }
    const cJSON* current = cJSON_GetObjectItem(json.get(), "current");
    const cJSON* daily = cJSON_GetObjectItem(json.get(), "daily");
    if (daily == nullptr) {
        return std::unexpected(std::string("Datos del tiempo incompletos"));
    }
    const cJSON* times = cJSON_GetObjectItem(daily, "time");
    const cJSON* codes = cJSON_GetObjectItem(daily, "weather_code");
    const cJSON* tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
    const cJSON* tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
    const cJSON* rain = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    const cJSON* wind = cJSON_GetObjectItem(daily, "wind_speed_10m_max");

    const int day = std::max(0, std::min(days_ahead, cJSON_GetArraySize(times) - 1));
    Report report;
    report.place = place.name;
    report.temp_min = Round(NumberAt(tmin, day));
    report.temp_max = Round(NumberAt(tmax, day));
    report.rain_probability = Round(NumberAt(rain, day));
    if (day == 0 && current != nullptr) {
        report.code = Round(cJSON_GetObjectItem(current, "weather_code")
                                ? cJSON_GetObjectItem(current, "weather_code")->valuedouble : NumberAt(codes, 0));
        const cJSON* t = cJSON_GetObjectItem(current, "temperature_2m");
        report.temperature = cJSON_IsNumber(t) ? Round(t->valuedouble) : report.temp_max;
        const cJSON* w = cJSON_GetObjectItem(current, "wind_speed_10m");
        report.wind = cJSON_IsNumber(w) ? Round(w->valuedouble) : Round(NumberAt(wind, 0));
        const cJSON* is_day = cJSON_GetObjectItem(current, "is_day");
        report.is_day = !cJSON_IsNumber(is_day) || is_day->valueint != 0;
    } else {
        report.code = Round(NumberAt(codes, day));
        report.temperature = report.temp_max;
        report.wind = Round(NumberAt(wind, day));
        report.is_day = true;
        const cJSON* date = cJSON_GetArrayItem(times, day);
        report.day_name = cJSON_IsString(date) ? date->valuestring : "";
    }
    return report;
}

// --- Textos -------------------------------------------------------------------------

const char* WeatherScreen::Describe(int code) {
    if (code == 0) return "Despejado";
    if (code <= 2) return "Poco nuboso";
    if (code == 3) return "Cubierto";
    if (code == 45 || code == 48) return "Niebla";
    if (code >= 51 && code <= 57) return "Llovizna";
    if (code >= 61 && code <= 64) return "Lluvia";
    if (code >= 65 && code <= 67) return "Lluvia fuerte";
    if (code >= 71 && code <= 77) return "Nieve";
    if (code >= 80 && code <= 82) return "Chubascos";
    if (code == 85 || code == 86) return "Chubascos de nieve";
    if (code >= 95) return "Tormenta";
    return "Nuboso";
}

const char* WeatherScreen::DescribeSpoken(int code) {
    if (code == 0) return "despejado";
    if (code <= 2) return "poco nuboso";
    if (code == 3) return "cubierto";
    if (code == 45 || code == 48) return "niebla";
    if (code >= 51 && code <= 57) return "llovizna";
    if (code >= 61 && code <= 64) return "lluvia";
    if (code >= 65 && code <= 67) return "lluvia fuerte";
    if (code >= 71 && code <= 77) return "nieve";
    if (code >= 80 && code <= 82) return "chubascos";
    if (code == 85 || code == 86) return "chubascos de nieve";
    if (code >= 95) return "tormenta";
    return "nuboso";
}

// --- Dibujo -------------------------------------------------------------------------

void WeatherScreen::Build() {
    if (root_ != nullptr) {
        return;
    }
    root_ = kit::Overlay();
    kit::Corners(root_);
    // Barra superior: lugar a la izquierda, cuándo a la derecha
    place_label_ = kit::Text(root_, 18, 8, 136, &font_mono_14, kit::kHudCyan, LV_TEXT_ALIGN_LEFT,
                             LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    when_label_ = kit::Text(root_, 150, 8, 72, &font_mono_14, kit::kHudDim, LV_TEXT_ALIGN_RIGHT);
    kit::HLine(root_, 18, 30, 204);

    icon_box_ = kit::Group(root_, 8, 32, 112, 108);
    temp_label_ = kit::Text(root_, 112, 52, 128, &font_mono_56, kit::kWhite, LV_TEXT_ALIGN_CENTER,
                            LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_letter_space(temp_label_, -2, 0);

    desc_label_ = kit::Centered(root_, 142, &font_mono_20, kit::kWhite, 224,
                                LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    kit::HLine(root_, 18, 172, 204);

    static const char* kTitles[4] = {"MÍN", "MÁX", "KM/H", "LLUVIA"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* title = kit::Text(root_, i * 60, 178, 60, &font_mono_14, kit::kHudDim);
        lv_label_set_text(title, kTitles[i]);
        values_[i] = kit::Text(root_, i * 60, 196, 60, &font_mono_20, kit::kWhite);
    }
    timer_ = lv_timer_create(&WeatherScreen::TimerCallback, 500, this);
}

void WeatherScreen::BuildIcon(int code, bool is_day) {
    lv_obj_clean(icon_box_);
    lv_obj_t* p = icon_box_;
    const bool clear = code == 0;
    const bool partly = code == 1 || code == 2;
    const bool overcast = code == 3;
    const bool fog = code == 45 || code == 48;
    const bool drizzle = code >= 51 && code <= 57;
    const bool rain = (code >= 61 && code <= 67) || (code >= 80 && code <= 82);
    const bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
    const bool storm = code >= 95;

    if (clear) {
        if (is_day) {
            Sun(p, 56, 54, 86);
        } else {
            Moon(p, 52, 54, 52);
        }
        return;
    }
    if (partly) {
        if (is_day) {
            Sun(p, 72, 36, 60);
        } else {
            Moon(p, 74, 34, 36);
        }
        Cloud(p, 8, 46, 100, kCloudWhite, 0);
        return;
    }
    if (fog) {
        Fog(p);
        return;
    }
    if (overcast) {
        Cloud(p, 32, 18, 85, kCloudGray, 600);
        Cloud(p, 8, 42, 100, kCloudWhite, 0);
        return;
    }
    // Lluvia, nieve y tormenta: nube arriba y cosas cayendo debajo
    const uint32_t cloud_color = storm ? kCloudDark : (rain ? kCloudGray : kCloudWhite);
    if (drizzle) {
        Drops(p, 3, 30, 22, 60, 96, 900, kRain, 4, 9);
    } else if (rain) {
        const bool heavy = code == 65 || code == 67 || code == 82;
        Drops(p, 5, 20, 16, 58, 96, heavy ? 500 : 750, kRain, 4, 12);
    } else if (snow) {
        Drops(p, 5, 20, 16, 58, 96, 1700, kSnow, 7, 7);
    } else if (storm) {
        Drops(p, 4, 16, 22, 58, 96, 600, kRain, 4, 12);
        Bolt(p, 44, 58);
    }
    Cloud(p, 14, 8, 115, cloud_color, 0);
}

void WeatherScreen::Show(const Report& report) {
    DisplayLockGuard lock(display_);
    Build();
    BuildIcon(report.code, report.is_day);

    std::string place = "\u25B8 " + kit::ScreenText(report.place, 40);
    lv_label_set_text(place_label_, place.c_str());
    lv_label_set_text(when_label_, report.day_name.empty() ? "AHORA" : DayName(report.day_name.c_str(), false));
    lv_label_set_text(desc_label_, Describe(report.code));

    // Temperatura grande con color según el frío o el calor
    auto temp_color = [](int t) -> uint32_t {
        return t <= 0 ? 0x7FD6FF : (t <= 15 ? 0xE6F7FF : (t <= 25 ? 0xFFD27A : 0xFF8A50));
    };
    char text[16];
    const int shown = std::max(-99, std::min(99, report.temperature));
    snprintf(text, sizeof(text), shown <= -10 ? "%d" : "%d\u00B0", shown);
    lv_label_set_text(temp_label_, text);
    kit::SetColor(temp_label_, temp_color(report.temperature));

    snprintf(text, sizeof(text), "%d\u00B0", report.temp_min);
    lv_label_set_text(values_[0], text);
    kit::SetColor(values_[0], 0x7FD6FF);
    snprintf(text, sizeof(text), "%d\u00B0", report.temp_max);
    lv_label_set_text(values_[1], text);
    kit::SetColor(values_[1], 0xFF8A50);
    snprintf(text, sizeof(text), "%d", report.wind);
    lv_label_set_text(values_[2], text);
    kit::SetColor(values_[2], report.wind >= 50 ? 0xFF6E6E : 0xE6F7FF);
    snprintf(text, sizeof(text), "%d%%", report.rain_probability);
    lv_label_set_text(values_[3], text);
    kit::SetColor(values_[3], 0x4FC3F7);

    visible_until_ = lv_tick_get() + kShowMs;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void WeatherScreen::TimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<WeatherScreen*>(lv_timer_get_user_data(timer));
    if (self->root_ == nullptr || lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (static_cast<int32_t>(lv_tick_get() - self->visible_until_) >= 0) {
        lv_obj_add_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(self->icon_box_);  // para las animaciones
    }
}

// --- Herramientas para la IA ----------------------------------------------------------

void WeatherScreen::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.weather.show",
        "Muestra en la pantalla del robot el tiempo con iconos animados y devuelve los datos para "
        "contárselos al usuario. Usa 'place' con el nombre de una ciudad o pueblo; si no se indica, "
        "usa la ubicación de casa. Para montañas o sitios sin nombre de ciudad, pasa sus coordenadas "
        "en 'latitude' y 'longitude' (y el nombre en 'place'). 'days_ahead' = 0 para ahora, 1 para "
        "mañana, hasta 6. Show the weather forecast with animated icons.",
        PropertyList({
            Property("place", kPropertyTypeString, std::string("")),
            Property("days_ahead", kPropertyTypeInteger, 0, 0, 6),
            Property("latitude", kPropertyTypeString, std::string("")),
            Property("longitude", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& p) -> ToolResult {
            std::string place_name = p["place"].value<std::string>();
            const std::string lat = p["latitude"].value<std::string>();
            const std::string lon = p["longitude"].value<std::string>();
            const int days_ahead = p["days_ahead"].value<int>();

            Place place;
            if (!lat.empty() && !lon.empty()) {
                place.latitude = std::strtod(lat.c_str(), nullptr);
                place.longitude = std::strtod(lon.c_str(), nullptr);
                place.name = place_name.empty() ? std::string("Ubicacion") : place_name;
            } else if (!place_name.empty()) {
                auto found = Geocode(place_name);
                if (!found) {
                    return std::unexpected(found.error());
                }
                place = *found;
            } else {
                Settings settings("weather", false);
                place.name = settings.GetString("name", "");
                if (place.name.empty()) {
                    return std::unexpected(std::string(
                        "No sé dónde vive el usuario. Pregúntale su ciudad y guárdala con "
                        "self.weather.set_home."));
                }
                place.latitude = std::strtod(settings.GetString("lat", "0").c_str(), nullptr);
                place.longitude = std::strtod(settings.GetString("lon", "0").c_str(), nullptr);
            }

            auto report = Fetch(place, days_ahead);
            if (!report) {
                return std::unexpected(report.error());
            }
            Show(*report);

            char text[320];
            if (report->day_name.empty()) {
                snprintf(text, sizeof(text),
                         "Tiempo ahora en %s: %d grados, %s. Mínima %d y máxima %d. Viento %d km/h. "
                         "Probabilidad de lluvia %d%%.",
                         report->place.c_str(), report->temperature, DescribeSpoken(report->code),
                         report->temp_min, report->temp_max, report->wind, report->rain_probability);
            } else {
                snprintf(text, sizeof(text),
                         "Previsión para el %s en %s: %s. Mínima %d y máxima %d grados. Viento de hasta "
                         "%d km/h. Probabilidad de lluvia %d%%.",
                         DayName(report->day_name.c_str(), true), report->place.c_str(),
                         DescribeSpoken(report->code), report->temp_min, report->temp_max, report->wind,
                         report->rain_probability);
            }
            return std::string(text);
        });

    mcp.AddTool(
        "self.weather.set_home",
        "Guarda la ciudad donde vive el usuario para consultar el tiempo sin decirla cada vez. "
        "Save the user's home city for weather.",
        PropertyList({Property("place", kPropertyTypeString)}),
        [this](const PropertyList& p) -> ToolResult {
            auto found = Geocode(p["place"].value<std::string>());
            if (!found) {
                return std::unexpected(found.error());
            }
            Settings settings("weather", true);
            char number[24];
            settings.SetString("name", found->name);
            snprintf(number, sizeof(number), "%.4f", found->latitude);
            settings.SetString("lat", number);
            snprintf(number, sizeof(number), "%.4f", found->longitude);
            settings.SetString("lon", number);
            return std::string("Ciudad guardada: ") + found->name;
        });

    ESP_LOGI(TAG, "Herramientas del tiempo registradas");
}
