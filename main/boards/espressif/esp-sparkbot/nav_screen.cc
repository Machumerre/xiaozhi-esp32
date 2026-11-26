// Navegador para el coche: el móvil (MacroDroid) reenvía al robot las indicaciones de Google Maps
// por wifi y el robot las dibuja en estilo HUD.
//
// El móvil llama a:   http://IP_DEL_ROBOT:8080/nav?t=<título>&x=<texto>&s=<subtexto>
// y al terminar a:    http://IP_DEL_ROBOT:8080/nav/end
#include "nav_screen.h"

#include "application.h"
#include "display/display.h"
#include "mcp_server.h"
#include "screen_kit.h"

#include <esp_log.h>
#include <esp_netif.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "Nav"

LV_FONT_DECLARE(font_nav_arrows);
LV_FONT_DECLARE(font_mono_48);

namespace {

constexpr uint16_t kPort = 8080;
constexpr uint32_t kShowMs = 60000;  // se oculta si en un minuto no llega nada

std::string Lower(const std::string& text) {
    std::string out = text;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool Has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }

std::string Trim(std::string text) {
    const char* spaces = " \t\r\n,.;:";
    text.erase(0, text.find_first_not_of(spaces));
    const size_t end = text.find_last_not_of(spaces);
    text.erase(end == std::string::npos ? 0 : end + 1);
    return text;
}

// Busca "250 m", "1,2 km"...; devuelve true si encuentra una distancia
bool FindDistance(const std::string& text, std::string& number, std::string& unit) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        if (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == ':')) continue;
        size_t j = i;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == ',' || text[j] == '.')) {
            ++j;
        }
        size_t k = j;
        while (k < text.size() && text[k] == ' ') ++k;
        auto letter_after = [&](size_t pos) {
            return pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos]));
        };
        if (text.compare(k, 2, "km") == 0 && !letter_after(k + 2)) {
            number = text.substr(i, j - i);
            unit = "km";
            return true;
        }
        if (k < text.size() && text[k] == 'm' && !letter_after(k + 1)) {
            number = text.substr(i, j - i);
            unit = "m";
            return true;
        }
        i = j;
    }
    return false;
}

std::string FindClock(const std::string& text) {
    for (size_t i = 0; i + 4 < text.size(); ++i) {
        if (text[i + 2] == ':' && std::isdigit(static_cast<unsigned char>(text[i])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 1])) && std::isdigit(static_cast<unsigned char>(text[i + 3])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 4]))) {
            return text.substr(i, 5);
        }
        if (text[i + 1] == ':' && std::isdigit(static_cast<unsigned char>(text[i])) &&
            std::isdigit(static_cast<unsigned char>(text[i + 2])) && std::isdigit(static_cast<unsigned char>(text[i + 3])) &&
            (i == 0 || !std::isdigit(static_cast<unsigned char>(text[i - 1])))) {
            return "0" + text.substr(i, 4);
        }
    }
    return "";
}

int RoundaboutExit(const std::string& lower) {
    const size_t at = lower.find("salida");
    if (at == std::string::npos) return 0;
    // "toma la 2.ª salida" / "la segunda salida" / "salida 3"
    for (size_t i = at; i-- > 0 && at - i < 8;) {
        if (std::isdigit(static_cast<unsigned char>(lower[i]))) return lower[i] - '0';
    }
    static const char* kWords[] = {"primera", "segunda", "tercera", "cuarta", "quinta", "sexta"};
    for (int n = 0; n < 6; ++n) {
        if (Has(lower, kWords[n])) return n + 1;
    }
    for (size_t i = at + 6; i < lower.size() && i < at + 10; ++i) {
        if (std::isdigit(static_cast<unsigned char>(lower[i]))) return lower[i] - '0';
    }
    return 0;
}

}  // namespace

NavScreen::NavScreen(Display* display) : display_(display) {
    // El servidor se arranca cuando ya hay red
    esp_timer_create_args_t args = {};
    args.callback = &NavScreen::StartTimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "nav_start";
    ESP_ERROR_CHECK(esp_timer_create(&args, &start_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(start_timer_, 5 * 1000000ULL));
}

// ---------------------------------------------------------------------------
// Servidor web
// ---------------------------------------------------------------------------

void NavScreen::StartTimerCallback(void* arg) {
    auto* self = static_cast<NavScreen*>(arg);
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateUnknown || state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
        state == kDeviceStateActivating) {
        return;
    }
    self->TryStartServer();
    if (self->server_ != nullptr) {
        esp_timer_stop(self->start_timer_);
    }
}

void NavScreen::TryStartServer() {
    if (server_ != nullptr) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.ctrl_port = 32790;
    config.max_uri_handlers = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    if (httpd_start(&server_, &config) != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo arrancar el servidor del navegador");
        server_ = nullptr;
        return;
    }
    httpd_uri_t end = {};
    end.uri = "/nav/end";
    end.method = HTTP_GET;
    end.handler = &NavScreen::HandleEnd;
    end.user_ctx = this;
    httpd_register_uri_handler(server_, &end);
    httpd_uri_t nav = {};
    nav.uri = "/nav";
    nav.method = HTTP_GET;
    nav.handler = &NavScreen::HandleNav;
    nav.user_ctx = this;
    httpd_register_uri_handler(server_, &nav);
    ESP_LOGI(TAG, "Navegador escuchando en http://%s:%u/nav", IpAddress().c_str(), kPort);
}

std::string NavScreen::QueryValue(const std::string& query, const char* key) {
    const std::string needle = std::string(key) + "=";
    size_t start = 0;
    while (start < query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        if (query.compare(start, needle.size(), needle) == 0) {
            std::string out;
            for (size_t i = start + needle.size(); i < end; ++i) {
                if (query[i] == '+') {
                    out.push_back(' ');
                } else if (query[i] == '%' && i + 2 < end &&
                           std::isxdigit(static_cast<unsigned char>(query[i + 1])) &&
                           std::isxdigit(static_cast<unsigned char>(query[i + 2]))) {
                    out.push_back(static_cast<char>(std::stoi(query.substr(i + 1, 2), nullptr, 16)));
                    i += 2;
                } else {
                    out.push_back(query[i]);
                }
            }
            return out.substr(0, 200);
        }
        start = end + 1;
    }
    return "";
}

esp_err_t NavScreen::HandleNav(httpd_req_t* req) {
    auto* self = static_cast<NavScreen*>(req->user_ctx);
    std::string query;
    const size_t length = httpd_req_get_url_query_len(req);
    if (length > 0 && length < 1500) {
        query.resize(length + 1);
        if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) query.clear();
        query.resize(strlen(query.c_str()));
    }
    const Step step = Parse(QueryValue(query, "t"), QueryValue(query, "x"), QueryValue(query, "s"));
    {
        std::lock_guard<std::mutex> guard(self->mutex_);
        self->updates_++;
    }
    Application::GetInstance().Schedule([self, step]() { self->Show(step); });
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t NavScreen::HandleEnd(httpd_req_t* req) {
    auto* self = static_cast<NavScreen*>(req->user_ctx);
    Application::GetInstance().Schedule([self]() { self->Hide(); });
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

std::string NavScreen::IpAddress() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }
    char text[20];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    return text;
}

// ---------------------------------------------------------------------------
// Interpretar la notificación de Google Maps (en español)
// ---------------------------------------------------------------------------

NavScreen::Step NavScreen::Parse(const std::string& title, const std::string& text, const std::string& sub) {
    Step step;
    const bool title_has_distance = FindDistance(title, step.distance, step.unit);
    if (!title_has_distance) {
        FindDistance(text, step.distance, step.unit);
    }
    std::string instruction = title_has_distance && !text.empty() ? text : title;
    if (!title_has_distance && instruction.empty()) instruction = text;

    // Calle: lo que va después de "hacia" o "por"
    std::string lower = Lower(instruction);
    for (const char* keyword : {" hacia ", " por ", " en dirección a "}) {
        const size_t at = lower.find(keyword);
        if (at != std::string::npos) {
            step.street = Trim(instruction.substr(at + strlen(keyword)));
            instruction = instruction.substr(0, at);
            break;
        }
    }
    // Quita la distancia del texto de la instrucción si va dentro ("En 300 m, gira...")
    if (!title_has_distance && !step.distance.empty()) {
        const size_t at = instruction.find(step.distance);
        if (at != std::string::npos && at < 6) {
            size_t cut = instruction.find_first_of(",", at);
            if (cut != std::string::npos) instruction = instruction.substr(cut + 1);
        }
    }
    step.instruction = Trim(instruction);
    if (!step.instruction.empty()) {
        step.instruction[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(step.instruction[0])));
    }

    const std::string all = Lower(title + " " + text);
    if (Has(all, "has llegado") || Has(all, "destino") || Has(all, "llegarás")) {
        step.turn = Turn::kArrive;
    } else if (Has(all, "cambio de sentido") || Has(all, "da la vuelta")) {
        step.turn = Turn::kUTurn;
    } else if (Has(all, "rotonda") || Has(all, "glorieta")) {
        step.turn = Turn::kRoundabout;
        step.roundabout_exit = RoundaboutExit(all);
    } else if ((Has(all, "ligeramente") || Has(all, "mantente") || Has(all, "carril") || Has(all, "sal ") ||
                Has(all, "salida")) && Has(all, "izquierda")) {
        step.turn = Turn::kSlightLeft;
    } else if ((Has(all, "ligeramente") || Has(all, "mantente") || Has(all, "carril") || Has(all, "sal ") ||
                Has(all, "salida")) && Has(all, "derecha")) {
        step.turn = Turn::kSlightRight;
    } else if (Has(all, "izquierda")) {
        step.turn = Turn::kLeft;
    } else if (Has(all, "derecha")) {
        step.turn = Turn::kRight;
    }

    // Hora de llegada y lo que queda
    step.eta = FindClock(sub);
    if (step.eta.empty()) step.eta = FindClock(text);
    std::string rest;
    size_t start = 0;
    while (start <= sub.size() && !sub.empty()) {
        size_t end = sub.find("·", start);
        if (end == std::string::npos) end = sub.size();
        const std::string part = Trim(sub.substr(start, end - start));
        if (!part.empty() && part.find(':') == std::string::npos) {
            if (!rest.empty()) rest += " · ";
            rest += part;
        }
        start = end + strlen("·");
        if (end == sub.size()) break;
    }
    step.remaining = rest;
    return step;
}

// ---------------------------------------------------------------------------
// Pantalla
// ---------------------------------------------------------------------------

void NavScreen::Build() {
    if (root_ != nullptr) return;
    root_ = kit::Overlay();
    kit::Corners(root_);
    lv_obj_t* title = kit::Text(root_, 18, 8, 108, &font_mono_14, kit::kHudDim, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(title, "▸ NAVEGACIÓN");
    eta_label_ = kit::Text(root_, 126, 8, 96, &font_mono_14, kit::kHudCyan, LV_TEXT_ALIGN_RIGHT);
    kit::HLine(root_, 18, 30, 204);

    // Flecha del giro (o rotonda) a la izquierda, distancia grande a la derecha
    arrow_label_ = kit::Text(root_, 8, 36, 104, &font_nav_arrows, kit::kHudCyan);
    ring_ = kit::Box(root_, 22, 44, 76, 76, LV_RADIUS_CIRCLE, 0x000000);
    lv_obj_set_style_border_width(ring_, 7, 0);
    lv_obj_set_style_border_color(ring_, lv_color_hex(kit::kHudCyan), 0);
    ring_exit_ = kit::Text(ring_, -7, 13, 76, &font_mono_36, 0xFFFFFF);
    distance_label_ = kit::Text(root_, 112, 46, 124, &font_mono_48, 0xFFFFFF);
    lv_obj_set_style_text_letter_space(distance_label_, -2, 0);
    unit_label_ = kit::Text(root_, 112, 100, 124, &font_mono_20, kit::kHudDim);

    instruction_label_ = kit::Centered(root_, 134, &font_mono_20, 0xFFFFFF, 224, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    street_label_ = kit::Centered(root_, 162, &font_mono_14, kit::kHudCyan, 224, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    kit::HLine(root_, 18, 188, 204);
    remaining_label_ = kit::Centered(root_, 198, &font_mono_14, kit::kHudDim, 210);

    hide_timer_ = lv_timer_create(&NavScreen::HideCallback, 1000, this);
}

void NavScreen::Show(const Step& step) {
    DisplayLockGuard lock(display_);
    Build();
    const char* arrow = "↑";
    uint32_t color = kit::kHudCyan;
    switch (step.turn) {
        case Turn::kLeft: arrow = "←"; break;
        case Turn::kRight: arrow = "→"; break;
        case Turn::kSlightLeft: arrow = "↖"; break;
        case Turn::kSlightRight: arrow = "↗"; break;
        case Turn::kUTurn: arrow = "↩"; color = 0xFFB300; break;
        case Turn::kArrive: arrow = "◆"; color = 0x4DFFA0; break;
        default: break;
    }
    if (step.turn == Turn::kRoundabout) {
        lv_obj_add_flag(arrow_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ring_, LV_OBJ_FLAG_HIDDEN);
        char exit_text[16] = "";
        if (step.roundabout_exit > 0) snprintf(exit_text, sizeof(exit_text), "%d", step.roundabout_exit);
        lv_label_set_text(ring_exit_, exit_text);
    } else {
        lv_obj_remove_flag(arrow_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ring_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(arrow_label_, arrow);
        kit::SetColor(arrow_label_, color);
    }

    // Cerca del giro el número se pone ámbar
    const bool close = step.unit == "m" && !step.distance.empty() && std::atoi(step.distance.c_str()) <= 100;
    lv_label_set_text(distance_label_, step.distance.empty() ? "" : step.distance.c_str());
    kit::SetColor(distance_label_, close ? 0xFFB300 : 0xFFFFFF);
    lv_label_set_text(unit_label_, step.distance.empty() ? "" : (step.unit == "km" ? "KM" : "METROS"));

    const std::string instruction = kit::ScreenText(step.instruction, 60);
    const std::string street = kit::ScreenText(step.street, 60);
    lv_label_set_text(instruction_label_, instruction.c_str());
    lv_label_set_text(street_label_, street.c_str());
    const std::string remaining = kit::ScreenText(step.remaining, 40);
    lv_label_set_text(remaining_label_, remaining.c_str());
    std::string eta = step.eta.empty() ? "" : "LLEGA " + step.eta;
    lv_label_set_text(eta_label_, eta.c_str());

    visible_until_ = lv_tick_get() + kShowMs;
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
}

void NavScreen::Hide() {
    DisplayLockGuard lock(display_);
    if (root_ != nullptr) lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
}

void NavScreen::HideCallback(lv_timer_t* timer) {
    auto* self = static_cast<NavScreen*>(lv_timer_get_user_data(timer));
    if (self->root_ != nullptr && !lv_obj_has_flag(self->root_, LV_OBJ_FLAG_HIDDEN) &&
        static_cast<int32_t>(lv_tick_get() - self->visible_until_) >= 0) {
        lv_obj_add_flag(self->root_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------
// Herramientas para la IA
// ---------------------------------------------------------------------------

void NavScreen::RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.nav.info",
        "Información para configurar el modo navegador (indicaciones de Google Maps en la pantalla del robot "
        "desde el móvil con MacroDroid): devuelve la IP del robot y la muestra en pantalla. Navigation setup info.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            const std::string ip = IpAddress();
            if (ip.empty()) {
                return std::unexpected(std::string("El robot no está conectado a una red wifi"));
            }
            char text[160];
            snprintf(text, sizeof(text), "http://%s:%u/nav", ip.c_str(), kPort);
            display_->ShowNotification(text, 30000);
            int updates = 0;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                updates = updates_;
            }
            return std::string("Dirección para MacroDroid: ") + text +
                   (server_ != nullptr ? " (servidor activo" : " (servidor aún no arrancado") +
                   ", indicaciones recibidas: " + std::to_string(updates) + ").";
        });

    mcp.AddTool(
        "self.nav.demo",
        "Enseña en la pantalla un ejemplo del modo navegador para probarlo. Show a navigation demo.",
        PropertyList(), [this](const PropertyList&) -> ToolResult {
            Show(Parse("250 m", "Gira a la derecha hacia Calle de Alcalá", "12 min · 5,3 km · Llegada: 18:42"));
            return std::string("Mostrando un ejemplo del navegador.");
        });
}
