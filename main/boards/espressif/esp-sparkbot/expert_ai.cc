// "Pregunta difícil": el robot consulta directamente a Gemini (y a Groq si falla).
// Las claves NO están en el código: GitHub las mete al compilar en secrets.h (desde los
// secretos del repositorio) o se pueden guardar en el robot (NVS, espacio "ai").
#include "expert_ai.h"

#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#include "web_search_format.h"

#include <cJSON.h>
#include <esp_log.h>

#include <memory>

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY ""
#endif
#ifndef GROQ_API_KEY
#define GROQ_API_KEY ""
#endif

#define TAG "ExpertAi"

namespace {

constexpr char kGeminiUrl[] =
    "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-latest:generateContent";
constexpr char kGroqUrl[] = "https://api.groq.com/openai/v1/chat/completions";
constexpr char kGroqModel[] = "openai/gpt-oss-120b";
constexpr size_t kMaxResponse = 64 * 1024;
constexpr size_t kMaxAnswer = 2400;

constexpr char kSystemPrompt[] =
    "Eres el cerebro experto de Jarvis, el asistente de un estudiante de tercero de Matemáticas en España. "
    "Piensa con cuidado y responde en español, correcto y riguroso. Tu respuesta se leerá en voz alta: "
    "nada de LaTeX, markdown, tablas ni símbolos raros; di las fórmulas con palabras (por ejemplo, "
    "'la integral de cero a uno de x al cuadrado'). Sé claro y directo, como máximo unas 170 palabras, "
    "salvo que te pidan una demostración completa paso a paso.";

struct JsonDeleter {
    void operator()(cJSON* json) const { cJSON_Delete(json); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

std::string Print(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    return out;
}

std::string Key(const char* nvs_key, const char* built_in) {
    Settings settings("ai", false);
    std::string key = settings.GetString(nvs_key, "");
    return key.empty() ? std::string(built_in) : key;
}

std::string Trim(std::string text) {
    if (text.size() > kMaxAnswer) {
        text = TruncateUtf8(text, kMaxAnswer) + "...";
    }
    return text;
}

}  // namespace

std::expected<std::string, std::string> ExpertAi::Post(const std::string& url, const std::string& header,
                                                       const std::string& header_value, const std::string& body) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(5) : nullptr;
    if (!http) {
        return std::unexpected(std::string("Sin conexión a internet"));
    }
    http->SetTimeout(40000);  // pensar lleva su tiempo
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader(header, header_value);
    http->SetContent(std::string(body));
    if (auto opened = http->Open("POST", url); !opened) {
        return std::unexpected("No se pudo conectar: " + opened.error().ToString());
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return std::unexpected("Respuesta no válida: " + status.error().ToString());
    }
    auto response = ReadBounded(*http, kMaxResponse);
    http->Close();
    if (*status != 200) {
        ESP_LOGW(TAG, "HTTP %d: %.300s", *status, response ? response->c_str() : "");
        if (*status == 429) {
            return std::unexpected(std::string("límite gratuito alcanzado por ahora"));
        }
        return std::unexpected("error HTTP " + std::to_string(*status));
    }
    return response;
}

std::expected<std::string, std::string> ExpertAi::AskGemini(const std::string& key, const std::string& question) {
    JsonPtr root(cJSON_CreateObject());
    cJSON* system = cJSON_AddObjectToObject(root.get(), "system_instruction");
    cJSON* system_parts = cJSON_AddArrayToObject(system, "parts");
    cJSON* system_part = cJSON_CreateObject();
    cJSON_AddStringToObject(system_part, "text", kSystemPrompt);
    cJSON_AddItemToArray(system_parts, system_part);

    cJSON* contents = cJSON_AddArrayToObject(root.get(), "contents");
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON* parts = cJSON_AddArrayToObject(message, "parts");
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", question.c_str());
    cJSON_AddItemToArray(parts, part);
    cJSON_AddItemToArray(contents, message);

    cJSON* config = cJSON_AddObjectToObject(root.get(), "generationConfig");
    cJSON_AddNumberToObject(config, "maxOutputTokens", 6000);  // incluye el razonamiento interno

    auto response = Post(kGeminiUrl, "x-goog-api-key", key, Print(root.get()));
    if (!response) {
        return std::unexpected("Gemini: " + response.error());
    }
    JsonPtr json(cJSON_Parse(response->c_str()));
    const cJSON* candidates = json ? cJSON_GetObjectItem(json.get(), "candidates") : nullptr;
    const cJSON* first = cJSON_GetArrayItem(candidates, 0);
    const cJSON* content = first ? cJSON_GetObjectItem(first, "content") : nullptr;
    const cJSON* answer_parts = content ? cJSON_GetObjectItem(content, "parts") : nullptr;
    std::string answer;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, answer_parts) {
        const cJSON* thought = cJSON_GetObjectItem(item, "thought");
        const cJSON* text = cJSON_GetObjectItem(item, "text");
        if (cJSON_IsTrue(thought) || !cJSON_IsString(text)) continue;
        answer += text->valuestring;
    }
    if (answer.empty()) {
        return std::unexpected(std::string("Gemini no devolvió respuesta"));
    }
    return Trim(answer);
}

std::expected<std::string, std::string> ExpertAi::AskGroq(const std::string& key, const std::string& question) {
    JsonPtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "model", kGroqModel);
    cJSON_AddNumberToObject(root.get(), "max_completion_tokens", 4000);
    cJSON* messages = cJSON_AddArrayToObject(root.get(), "messages");
    cJSON* system = cJSON_CreateObject();
    cJSON_AddStringToObject(system, "role", "system");
    cJSON_AddStringToObject(system, "content", kSystemPrompt);
    cJSON_AddItemToArray(messages, system);
    cJSON* user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", question.c_str());
    cJSON_AddItemToArray(messages, user);

    auto response = Post(kGroqUrl, "Authorization", "Bearer " + key, Print(root.get()));
    if (!response) {
        return std::unexpected("Groq: " + response.error());
    }
    JsonPtr json(cJSON_Parse(response->c_str()));
    const cJSON* choices = json ? cJSON_GetObjectItem(json.get(), "choices") : nullptr;
    const cJSON* first = cJSON_GetArrayItem(choices, 0);
    const cJSON* message = first ? cJSON_GetObjectItem(first, "message") : nullptr;
    const cJSON* content = message ? cJSON_GetObjectItem(message, "content") : nullptr;
    if (!cJSON_IsString(content) || content->valuestring[0] == '\0') {
        return std::unexpected(std::string("Groq no devolvió respuesta"));
    }
    return Trim(content->valuestring);
}

void ExpertAi::RegisterTools() {
    const bool has_gemini = !Key("gemini_key", GEMINI_API_KEY).empty();
    const bool has_groq = !Key("groq_key", GROQ_API_KEY).empty();
    if (!has_gemini && !has_groq) {
        ESP_LOGW(TAG, "Sin claves de Gemini ni Groq: la herramienta de preguntas difíciles queda desactivada");
        return;
    }

    McpServer::GetInstance().AddTool(
        "self.ai.ask_expert",
        "Consulta a un modelo de IA experto que razona paso a paso (Gemini). Úsalo para preguntas difíciles: "
        "matemáticas avanzadas, demostraciones, física, programación, dudas de estudio complejas o cuando no "
        "estés seguro de la respuesta. Pasa en 'question' la pregunta completa con todo el contexto necesario. "
        "Después cuenta la respuesta al usuario con tus palabras, sin acortarla demasiado. Tarda unos segundos. "
        "Ask an expert reasoning model.",
        PropertyList({Property("question", kPropertyTypeString)}),
        [this](const PropertyList& p) -> ToolResult {
            const std::string question = p["question"].value<std::string>();
            if (question.empty()) {
                return std::unexpected(std::string("Falta la pregunta"));
            }
            std::string errors;
            const std::string gemini_key = Key("gemini_key", GEMINI_API_KEY);
            if (!gemini_key.empty()) {
                auto answer = AskGemini(gemini_key, question);
                if (answer) return *answer;
                errors = answer.error();
                ESP_LOGW(TAG, "%s", errors.c_str());
            }
            const std::string groq_key = Key("groq_key", GROQ_API_KEY);
            if (!groq_key.empty()) {
                auto answer = AskGroq(groq_key, question);
                if (answer) return *answer;
                errors += (errors.empty() ? "" : "; ") + answer.error();
            }
            return std::unexpected("No he podido consultar al experto (" + errors + ")");
        });
    ESP_LOGI(TAG, "Preguntas difíciles activadas (Gemini: %s, Groq: %s)", has_gemini ? "sí" : "no",
             has_groq ? "sí" : "no");
}
