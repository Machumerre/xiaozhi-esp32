// Agrupa varias herramientas MCP en una sola con un parámetro "action".
// El servidor de Xiaozhi admite un número limitado de herramientas por dispositivo, así que
// "self.alarm.set", "self.alarm.list"... se publican como una única "self.alarm".
//
// Uso: cambiar `auto& mcp = McpServer::GetInstance();` por `ToolGroup mcp("self.alarm", "resumen");`
// y dejar las llamadas a mcp.AddTool(...) como estaban. Al destruirse, registra la herramienta.
#ifndef TOOL_GROUP_H
#define TOOL_GROUP_H

#include "mcp_server.h"

#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ToolGroup {
public:
    using Handler = std::function<ToolResult(const PropertyList&)>;

    ToolGroup(std::string name, std::string summary) : name_(std::move(name)), summary_(std::move(summary)) {}
    ToolGroup(const ToolGroup&) = delete;
    ToolGroup& operator=(const ToolGroup&) = delete;
    ~ToolGroup() { Commit(); }

    void AddTool(const std::string& full_name, const std::string& description, PropertyList properties,
                 Handler handler) {
        Action action;
        const size_t dot = full_name.rfind('.');
        action.name = dot == std::string::npos ? full_name : full_name.substr(dot + 1);
        action.description = FirstSentences(description);
        for (auto& property : properties) {
            action.parameters.push_back(property.name());
            Merge(property);
        }
        action.handler = std::move(handler);
        actions_->push_back(std::move(action));
    }

    void Commit() {
        if (committed_ || actions_->empty()) {
            return;
        }
        committed_ = true;
        std::string description = summary_ + " Elige la acción con el parámetro 'action' (rellena solo los "
                                             "parámetros que use esa acción):";
        std::string names;
        for (const auto& action : *actions_) {
            description += " [" + action.name;
            if (!action.parameters.empty()) {
                description += "(";
                for (size_t i = 0; i < action.parameters.size(); ++i) {
                    description += (i ? ", " : "") + action.parameters[i];
                }
                description += ")";
            }
            description += "] " + action.description;
            names += (names.empty() ? "" : ", ") + action.name;
        }
        std::vector<Property> all;
        all.emplace_back("action", kPropertyTypeString);
        for (const auto& property : merged_) {
            all.push_back(property);
        }
        auto actions = actions_;
        McpServer::GetInstance().AddTool(
            name_, description, PropertyList(all), [actions, names](const PropertyList& p) -> ToolResult {
                std::string wanted = p["action"].value<std::string>();
                for (auto& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const auto& action : *actions) {
                    if (action.name == wanted) {
                        return action.handler(p);
                    }
                }
                return std::unexpected("Acción desconocida '" + wanted + "'. Usa una de: " + names);
            });
    }

private:
    struct Action {
        std::string name;
        std::string description;
        std::vector<std::string> parameters;
        Handler handler;
    };

    std::string name_;
    std::string summary_;
    std::shared_ptr<std::vector<Action>> actions_ = std::make_shared<std::vector<Action>>();
    std::vector<Property> merged_;
    bool committed_ = false;

    // Todos los parámetros pasan a ser opcionales: cada acción comprueba los suyos
    void Merge(const Property& property) {
        for (const auto& existing : merged_) {
            if (existing.name() == property.name()) {
                return;
            }
        }
        if (property.has_default_value()) {
            merged_.push_back(property);
        } else if (property.type() == kPropertyTypeString) {
            merged_.emplace_back(property.name(), kPropertyTypeString, std::string(""));
        } else if (property.type() == kPropertyTypeInteger && property.has_range()) {
            merged_.emplace_back(property.name(), kPropertyTypeInteger, property.min_value(), property.min_value(),
                                 property.max_value());
        } else if (property.type() == kPropertyTypeInteger) {
            merged_.emplace_back(property.name(), kPropertyTypeInteger, 0);
        } else {
            merged_.emplace_back(property.name(), kPropertyTypeBoolean, false);
        }
    }

    // Quita la coletilla en inglés del final para que la descripción no sea enorme
    static std::string FirstSentences(const std::string& text) {
        std::string out = text;
        for (const char* tail : {" Set ", " List ", " Remove ", " Stop ", " Snooze ", " Add ", " Show ", " Save ",
                                 " Recall ", " Forget ", " Study ", " Ask ", " Navigation ", " Configure ", " Pause ", " Get "}) {
            const size_t at = out.rfind(tail);
            if (at != std::string::npos && at > out.size() / 2) {
                out.erase(at);
                break;
            }
        }
        return out;
    }
};

#endif  // TOOL_GROUP_H
