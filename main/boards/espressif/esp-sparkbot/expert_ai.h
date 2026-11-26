// "Pregunta difícil": el robot consulta directamente a Gemini (y a Groq si falla).
#ifndef EXPERT_AI_H
#define EXPERT_AI_H

#include <expected>
#include <string>

class ExpertAi {
public:
    void RegisterTools();

private:
    std::expected<std::string, std::string> AskGemini(const std::string& key, const std::string& question);
    std::expected<std::string, std::string> AskGroq(const std::string& key, const std::string& question);
    static std::expected<std::string, std::string> Post(const std::string& url, const std::string& header,
                                                        const std::string& header_value, const std::string& body);
};

#endif  // EXPERT_AI_H
