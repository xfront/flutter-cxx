/// @file design_codegen.cpp
/// @brief CodeGenerator implementation — DesignModel → compilable C++ source.

#include "design_codegen.h"
#include "design_model.h"

#include <set>
#include <sstream>
#include "cui/foundation/shared_ptr.h"

namespace cui {
namespace designer {

std::string CodeGenerator::indent(int level) const {
    return std::string(level * 4, ' ');
}

std::string CodeGenerator::varNameFor(const DesignNode& node) const {
    // Use the node id directly (e.g. "button_1", "label_2").
    return node.id;
}

// ── Include generation ─────────────────────────────────────────────────────

void CodeGenerator::emitIncludes(const DesignModel& model,
                                  std::string& out) const {
    // Collect all widget types used in the tree.
    std::set<std::string> types;
    auto nodes = const_cast<DesignModel&>(model).flatten();
    for (auto* n : nodes)
        types.insert(n->widgetType);

    // Map widget type → header file.
    auto header = [](const std::string& type) -> std::string {
        // Convert CamelCase to snake_case.
        std::string result;
        for (size_t i = 0; i < type.size(); ++i) {
            if (i > 0 && type[i] >= 'A' && type[i] <= 'Z')
                result += '_';
            char c = type[i];
            if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
            result += c;
        }
        return "<cui_core/" + result + ".h>";
    };

    out += "#include <cui_core/foundation/application.h>\n";
    out += "#include <cui_core/foundation/root_window.h>\n";

    std::set<std::string> emitted;
    for (auto& t : types) {
        if (t == "Column" || t == "Row") {
            if (emitted.insert("flex").second)
                out += "#include <cui_core/widgets/layout/flex.h>\n";
        } else if (t == "VBoxContainer" || t == "HBoxContainer") {
            if (emitted.insert("container").second)
                out += "#include <cui_core/widgets/layout/container.h>\n";
        } else {
            auto h = header(t);
            if (emitted.insert(h).second)
                out += "#include " + h + "\n";
        }
    }
    out += "\n";
}

// ── Property emission ──────────────────────────────────────────────────────

void CodeGenerator::emitProperties(const DesignNode& node,
                                    const std::string& var,
                                    std::string& out) const {
    auto* info = findWidgetType(node.widgetType);
    if (!info) return;

    for (auto& pm : info->props) {
        auto it = node.properties.find(pm.name);
        if (it == node.properties.end()) continue;
        const std::string& val = it->second;

        // Skip layout props for the root container (it fills the window).
        if (node.id == "root" &&
            (pm.name == "x" || pm.name == "y" ||
             pm.name == "width" || pm.name == "height"))
            continue;

        if (pm.name == "text") {
            out += var + "->setText(\"" + val + "\");\n";
        } else if (pm.name == "x") {
            // Position is handled by absolute layout; skip for flex children.
        } else if (pm.name == "y") {
            // Same as x.
        } else if (pm.name == "width" || pm.name == "height") {
            // Skip — sizing handled by layout.
        } else if (pm.name == "visible") {
            if (val != "true")
                out += var + "->setVisible(false);\n";
        } else if (pm.name == "checked") {
            if (val == "true" || val == "1")
                out += var + "->setChecked(true);\n";
        } else if (pm.name == "spacing") {
            out += var + "->setSpacing(" + val + ");\n";
        } else if (pm.name == "padding") {
            out += var + "->setPadding(" + val + ");\n";
        } else if (pm.name == "fontSize") {
            // Not a universal setter; skip for now.
        }
    }
}

// ── Node emission ──────────────────────────────────────────────────────────

void CodeGenerator::emitNode(const DesignNode& node,
                              const std::string& parentId,
                              std::string& out, int ind) const {
    std::string var = varNameFor(node);
    std::string type = node.widgetType;

    // Construct the widget.
    if (type == "VBoxContainer") {
        double sp = node.propDouble("spacing", 8);
        double pd = node.propDouble("padding", 8);
        out += indent(ind) + "auto " + var + " = cui_core::make_shared<VBoxContainer>("
             + std::to_string(sp) + ", " + std::to_string(pd) + ");\n";
    } else if (type == "HBoxContainer") {
        double sp = node.propDouble("spacing", 8);
        double pd = node.propDouble("padding", 8);
        out += indent(ind) + "auto " + var + " = cui_core::make_shared<HBoxContainer>("
             + std::to_string(sp) + ", " + std::to_string(pd) + ");\n";
    } else if (type == "Column") {
        double sp = node.propDouble("spacing", 8);
        out += indent(ind) + "auto " + var
             + " = cui_core::make_shared<Column>(MainAxisAlignment::Start, "
               "CrossAxisAlignment::Stretch, "
             + std::to_string(sp) + ");\n";
    } else if (type == "Row") {
        double sp = node.propDouble("spacing", 8);
        out += indent(ind) + "auto " + var
             + " = cui_core::make_shared<Row>(MainAxisAlignment::Start, "
               "CrossAxisAlignment::Center, "
             + std::to_string(sp) + ");\n";
    } else {
        out += indent(ind) + "auto " + var + " = cui_core::make_shared<" + type + ">();\n";
    }

    // Emit property setters.
    emitProperties(node, var, out);

    // Add to parent.
    if (!parentId.empty()) {
        out += indent(ind) + parentId + "->addChild(" + var + ");\n";
    }

    out += "\n";

    // Recurse into children.
    for (auto& child : node.children)
        emitNode(*child, var, out, ind);
}

// ── Top-level generation ───────────────────────────────────────────────────

std::string CodeGenerator::generate(const DesignModel& model) const {
    std::string out;

    // Header comment.
    out += "// Generated by CUI Visual Designer\n";
    out += "// Do not edit manually — changes will be overwritten.\n\n";

    // Includes.
    emitIncludes(model, out);

    // main().
    out += "int main() {\n";
    out += "    using namespace cui_core;\n";
    out += "\n";
    out += "    Application app;\n";
    out += "    app.createWindow(\"My App\", 800, 600);\n";
    out += "\n";

    // Root container.
    auto* root = model.root();
    if (root) {
        std::string rootVar = varNameFor(*root);

        // Emit root construction.
        if (root->widgetType == "Column") {
            double sp = root->propDouble("spacing", 8);
            out += "    auto " + rootVar
                 + " = cui_core::make_shared<Column>(MainAxisAlignment::Start, "
                   "CrossAxisAlignment::Stretch, "
                 + std::to_string(sp) + ");\n";
        } else if (root->widgetType == "Row") {
            double sp = root->propDouble("spacing", 8);
            out += "    auto " + rootVar
                 + " = cui_core::make_shared<Row>(MainAxisAlignment::Start, "
                   "CrossAxisAlignment::Center, "
                 + std::to_string(sp) + ");\n";
        } else {
            out += "    auto " + rootVar + " = cui_core::make_shared<"
                 + root->widgetType + ">();\n";
        }
        out += "\n";

        // Emit children.
        for (auto& child : root->children)
            emitNode(*child, rootVar, out, 1);

        // Mount the generated tree as the application root.
        out += "    app.setRoot(" + rootVar + ");\n";
    }

    out += "\n";
    out += "    return app.run();\n";
    out += "}\n";

    return out;
}

}  // namespace designer
}  // namespace cui_core
