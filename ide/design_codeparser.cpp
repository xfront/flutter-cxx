/// @file design_codeparser.cpp
/// @brief CodeParser implementation — line-scanning C++ → DesignModel.

#include "design_codeparser.h"

#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "../cui_core/include/cui/utils/shared_ptr.h"
#include "design_model.h"

namespace cui {
namespace designer {

// ── Helpers ────────────────────────────────────────────────────────────────

namespace {

/// Trim whitespace from both ends.
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/// Extract the string literal content from `"..."` (no escape handling).
std::string extractStringLiteral(const std::string& s) {
    auto a = s.find('"');
    if (a == std::string::npos) return "";
    auto b = s.find('"', a + 1);
    if (b == std::string::npos) return "";
    return s.substr(a + 1, b - a - 1);
}

/// Map setter name → property key.
std::string setterToProperty(const std::string& setter) {
    if (setter == "setText") return "text";
    if (setter == "setVisible") return "visible";
    if (setter == "setChecked") return "checked";
    if (setter == "setSpacing") return "spacing";
    if (setter == "setPadding") return "padding";
    if (setter == "setFontSize") return "fontSize";
    return "";
}

/// Check if a type name is a known CUI widget type.
bool isKnownType(const std::string& type) {
    return findWidgetType(type) != nullptr;
}

}  // namespace

// ── Parse ──────────────────────────────────────────────────────────────────

bool CodeParser::parse(const std::string& source, DesignModel& model,
                        std::vector<ParseError>& errors) const {
    errors.clear();

    // Temporary structures for the parsed tree.
    struct ParsedNode {
        std::string varName;
        std::string widgetType;
        std::map<std::string, std::string> properties;
        std::string parentVar;  // from addChild
        int order = 0;          // creation order
    };

    std::map<std::string, ParsedNode> nodes;  // varName → node
    std::map<std::string, std::string> addChildMap;  // childVar → parentVar
    int orderCounter = 0;
    std::string rootVar;

    // Line-by-line scan.
    std::istringstream stream(source);
    std::string line;
    int lineNum = 0;

    // Regex patterns.
    // auto <var> = cui_core::make_shared<<Type>>(<args>);
    std::regex reCreate(
        R"(auto\s+(\w+)\s*=\s*cui_core::make_shared<(\w+)>\s*\(([^)]*)\)\s*;)");
    // <var>-><setter>(<value>);
    std::regex reSetter(
        R"((\w+)\s*->\s*(\w+)\s*\((.+)\)\s*;)");
    // <parent>->addChild(<child>);
    std::regex reAddChild(
        R"((\w+)\s*->\s*addChild\s*\(\s*(\w+)\s*\)\s*;)");

    while (std::getline(stream, line)) {
        ++lineNum;
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '/' || trimmed[0] == '#')
            continue;

        std::smatch match;

        // 1. Creation: auto <var> = cui_core::make_shared<<Type>>(...);
        if (std::regex_search(trimmed, match, reCreate)) {
            std::string var = match[1].str();
            std::string type = match[2].str();
            std::string args = match[3].str();

            if (!isKnownType(type)) continue;

            ParsedNode pn;
            pn.varName = var;
            pn.widgetType = type;
            pn.order = orderCounter++;

            // Extract spacing/padding from constructor args if present.
            if (type == "VBoxContainer" || type == "HBoxContainer") {
                // Args: spacing, padding
                std::regex reArgs(R"((\d+\.?\d*)\s*,\s*(\d+\.?\d*))");
                std::smatch m;
                if (std::regex_search(args, m, reArgs)) {
                    pn.properties["spacing"] = m[1].str();
                    pn.properties["padding"] = m[2].str();
                }
            } else if (type == "Column" || type == "Row") {
                // Args: mainAxisAlignment, crossAxisAlignment, spacing
                // Extract spacing (third arg — last number).
                std::regex reSp(R"(,\s*(\d+\.?\d*)\s*\)?)");
                std::smatch m;
                std::string search = args;
                std::string lastNum;
                while (std::regex_search(search, m, reSp)) {
                    lastNum = m[1].str();
                    search = m.suffix().str();
                }
                if (!lastNum.empty())
                    pn.properties["spacing"] = lastNum;
            }

            nodes[var] = std::move(pn);
            continue;
        }

        // 2. Setter: <var>-><setter>(<value>);
        if (std::regex_search(trimmed, match, reSetter)) {
            std::string var = match[1].str();
            std::string setter = match[2].str();
            std::string value = trim(match[3].str());

            std::string propKey = setterToProperty(setter);
            if (propKey.empty()) continue;

            auto it = nodes.find(var);
            if (it == nodes.end()) continue;

            // Parse value.
            if (value.front() == '"') {
                it->second.properties[propKey] = extractStringLiteral(value);
            } else if (value == "true" || value == "false") {
                it->second.properties[propKey] = value;
            } else {
                // Numeric or other.
                it->second.properties[propKey] = value;
            }
            continue;
        }

        // 3. addChild: <parent>->addChild(<child>);
        if (std::regex_search(trimmed, match, reAddChild)) {
            std::string parent = match[1].str();
            std::string child = match[2].str();
            addChildMap[child] = parent;
            continue;
        }
    }

    // If no nodes found, report error.
    if (nodes.empty()) {
        errors.push_back({0, "No CUI widget constructions found in source"});
        return false;
    }

    // Build a temporary DesignModel from the parsed data.
    // First, find the root node (the one that is never a child of another).
    // The root is typically the one passed to setContent().
    // Heuristic: the node with the lowest order that is a container.
    // Better: look for setContent(<var>) pattern.
    std::regex reSetContent(R"(setContent\s*\(\s*(\w+)\s*\))");
    for (auto& [var, pn] : nodes) {
        // Check if this var appears in a setContent call.
        if (source.find("setContent(" + var + ")") != std::string::npos) {
            rootVar = var;
            break;
        }
    }
    // Fallback: first container node.
    if (rootVar.empty()) {
        for (auto& [var, pn] : nodes) {
            if (isKnownType(pn.widgetType) &&
                findWidgetType(pn.widgetType) &&
                DesignNode::create(pn.widgetType, 0)->isContainer()) {
                rootVar = var;
                break;
            }
        }
    }
    if (rootVar.empty()) {
        rootVar = nodes.begin()->first;
    }

    // Rebuild the model.
    // We need to clear the existing model and rebuild it.
    // Since DesignModel doesn't have a reset(), we'll work around by
    // removing all children from root and rebuilding.
    auto* modelRoot = model.root();
    // Clear existing children.
    while (!modelRoot->children.empty())
        model.removeNode(modelRoot->children.back().get());

    // Change root's type if needed.
    modelRoot->widgetType = nodes[rootVar].widgetType;
    modelRoot->id = rootVar;
    modelRoot->displayName = rootVar + " (" + nodes[rootVar].widgetType + ")";
    for (auto& [k, v] : nodes[rootVar].properties)
        modelRoot->properties[k] = v;

    // Build child nodes recursively.
    std::function<void(const std::string&, DesignNode*)> buildChildren;
    buildChildren = [&](const std::string& parentVar, DesignNode* parentNode) {
        // Find all nodes whose parent is parentVar.
        std::vector<std::pair<int, std::string>> ordered;  // (order, varName)
        for (auto& [var, pn] : nodes) {
            auto it = addChildMap.find(var);
            if (it != addChildMap.end() && it->second == parentVar)
                ordered.push_back({pn.order, var});
        }
        // Sort by creation order.
        std::sort(ordered.begin(), ordered.end());

        for (auto& [ord, var] : ordered) {
            auto& pn = nodes[var];
            auto* child = model.addChild(parentNode, pn.widgetType);
            child->id = var;
            child->displayName = var + " (" + pn.widgetType + ")";
            for (auto& [k, v] : pn.properties)
                child->properties[k] = v;
            buildChildren(var, child);
        }
    };

    buildChildren(rootVar, modelRoot);
    model.notifyChanged();

    return true;
}

}  // namespace designer
}  // namespace cui_core
