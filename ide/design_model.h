#pragma once
/// @file design_model.h
/// @brief Core data model for the CUI visual designer.
///
/// DesignNode represents one widget in the design tree.
/// DesignModel owns the root node, manages selection, and fires change
/// notifications so that the canvas, property editor, hierarchy tree,
/// and code generator all stay in sync.

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cui {
namespace designer {

// ── Property metadata ──────────────────────────────────────────────────────

enum class PropKind { String, Int, Double, Bool, Color, Enum };

struct PropMeta {
    std::string name;       // "text", "width", "visible" …
    PropKind kind = PropKind::String;
    std::string defaultVal;
    std::vector<std::string> enumValues;  // when kind == Enum
    std::string group;      // "Layout", "Appearance", "Content", "Behavior"
};

// ── 控件类型注册表 ─────────────────────────────────────────────────────────────

struct WidgetTypeInfo {
    std::string type;           // "Button", "Text", …
    std::string category;       // "Basic", "Container", "Data", "Decoration"
    std::vector<PropMeta> props;
};

/// Return the global widget-type registry (lazily initialised).
inline const std::vector<WidgetTypeInfo>& widgetTypeRegistry() {
    static std::vector<WidgetTypeInfo> reg = [] {
        std::vector<WidgetTypeInfo> v;
        auto S = [](PropKind k) { return k; };
        (void)S;

        // Helper to make a PropMeta quickly
        auto P = [](const char* n, PropKind k, const char* def,
                    const char* grp = "Content") -> PropMeta {
            return {n, k, def, {}, grp};
        };

        // ── Basic ────────────────────────────────────────────────────────
        v.push_back({"Button", "Basic", {
            P("text", PropKind::String, "Button"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "100", "Layout"),
            P("height", PropKind::Int, "32", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Text", "Basic", {
            P("text", PropKind::String, "Text"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "80", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("fontSize", PropKind::Int, "13", "Appearance"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"TextField", "Basic", {
            P("text", PropKind::String, ""),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "160", "Layout"),
            P("height", PropKind::Int, "30", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Checkbox", "Basic", {
            P("text", PropKind::String, "Checkbox"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "120", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("checked", PropKind::Bool, "false", "Content"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Radio", "Basic", {
            P("text", PropKind::String, "Radio"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "120", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Switch", "Basic", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "48", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("checked", PropKind::Bool, "false", "Content"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Slider", "Basic", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "160", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"SpinBox", "Basic", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "100", "Layout"),
            P("height", PropKind::Int, "30", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"DropdownButton", "Basic", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "140", "Layout"),
            P("height", PropKind::Int, "30", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});

        // ── Container ────────────────────────────────────────────────────
        v.push_back({"VBoxContainer", "Container", {
            P("spacing", PropKind::Double, "8", "Layout"),
            P("padding", PropKind::Double, "8", "Layout"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "200", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"HBoxContainer", "Container", {
            P("spacing", PropKind::Double, "8", "Layout"),
            P("padding", PropKind::Double, "8", "Layout"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "60", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Column", "Container", {
            P("spacing", PropKind::Double, "8", "Layout"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "200", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Row", "Container", {
            P("spacing", PropKind::Double, "8", "Layout"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "60", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Stack", "Container", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "200", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Card", "Container", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "120", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"ScrollView", "Container", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "200", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});

        // ── Data ─────────────────────────────────────────────────────────
        v.push_back({"TableView", "Data", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "240", "Layout"),
            P("height", PropKind::Int, "160", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"TreeView", "Data", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "160", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"ListView", "Data", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "160", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"TabWidget", "Data", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "300", "Layout"),
            P("height", PropKind::Int, "200", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});

        // ── Decoration ───────────────────────────────────────────────────
        v.push_back({"Divider", "Decoration", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "200", "Layout"),
            P("height", PropKind::Int, "1", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Spacer", "Decoration", {
            P("width", PropKind::Int, "20", "Layout"),
            P("height", PropKind::Int, "20", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"LinearProgressIndicator", "Decoration", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "160", "Layout"),
            P("height", PropKind::Int, "20", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"RatingBar", "Decoration", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "120", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Badge", "Decoration", {
            P("text", PropKind::String, "1"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "24", "Layout"),
            P("height", PropKind::Int, "24", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"CircleAvatar", "Decoration", {
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "40", "Layout"),
            P("height", PropKind::Int, "40", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});
        v.push_back({"Chip", "Decoration", {
            P("text", PropKind::String, "Chip"),
            P("x", PropKind::Int, "20", "Layout"),
            P("y", PropKind::Int, "20", "Layout"),
            P("width", PropKind::Int, "80", "Layout"),
            P("height", PropKind::Int, "28", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});

        // ── Layout ───────────────────────────────────────────────────────
        v.push_back({"SizedBox", "Layout", {
            P("width", PropKind::Int, "100", "Layout"),
            P("height", PropKind::Int, "100", "Layout"),
            P("visible", PropKind::Bool, "true", "Behavior"),
        }});

        return v;
    }();
    return reg;
}

/// Look up type info by widget type name; returns nullptr if not found.
inline const WidgetTypeInfo* findWidgetType(const std::string& type) {
    for (auto& t : widgetTypeRegistry())
        if (t.type == type) return &t;
    return nullptr;
}

// ── DesignNode ─────────────────────────────────────────────────────────────

struct DesignNode {
    std::string id;           // "button_1", "label_2" …
    std::string widgetType;   // "Button", "Text" …
    std::string displayName;  // shown in hierarchy tree

    // Generic property table (key → string value).
    std::map<std::string, std::string> properties;

    // Tree structure.
    DesignNode* parent = nullptr;
    std::vector<std::unique_ptr<DesignNode>> children;

    // ── Helpers ──────────────────────────────────────────────────────────

    bool isContainer() const {
        static const std::set<std::string> containers = {
            "VBoxContainer", "HBoxContainer", "Column", "Row",
            "Stack", "Card", "ScrollView", "TabWidget", "SizedBox",
        };
        return containers.count(widgetType) > 0;
    }

    std::string prop(const std::string& key,
                     const std::string& fallback = "") const {
        auto it = properties.find(key);
        return it != properties.end() ? it->second : fallback;
    }

    int propInt(const std::string& key, int fallback = 0) const {
        auto it = properties.find(key);
        if (it == properties.end()) return fallback;
        try { return std::stoi(it->second); } catch (...) { return fallback; }
    }

    double propDouble(const std::string& key, double fallback = 0) const {
        auto it = properties.find(key);
        if (it == properties.end()) return fallback;
        try { return std::stod(it->second); } catch (...) { return fallback; }
    }

    bool propBool(const std::string& key, bool fallback = true) const {
        auto it = properties.find(key);
        if (it == properties.end()) return fallback;
        return it->second == "true" || it->second == "1";
    }

    /// Factory: create a node of the given widget type with default props.
    static std::unique_ptr<DesignNode> create(const std::string& type,
                                              int uniqueId) {
        auto node = std::make_unique<DesignNode>();
        node->widgetType = type;
        node->id = type + "_" + std::to_string(uniqueId);
        node->displayName = node->id;

        // Fill default property values from the registry.
        if (auto* info = findWidgetType(type)) {
            for (auto& pm : info->props)
                node->properties[pm.name] = pm.defaultVal;
        }
        return node;
    }
};

// ── DesignModel ────────────────────────────────────────────────────────────

class DesignModel {
public:
    DesignModel() {
        // Create a root Column container.
        root_ = DesignNode::create("Column", nextId_++);
        root_->id = "root";
        root_->displayName = "root (Column)";
        root_->properties["spacing"] = "8";
        root_->properties["x"] = "0";
        root_->properties["y"] = "0";
        root_->properties["width"] = "600";
        root_->properties["height"] = "400";
    }

    // ── Tree access ────────────────────────────────────────────────────
    DesignNode* root() { return root_.get(); }
    const DesignNode* root() const { return root_.get(); }

    // ── Selection ──────────────────────────────────────────────────────
    DesignNode* selected() const { return selected_; }

    void select(DesignNode* node) {
        selected_ = node;
        multiSelection_.clear();
        if (node) multiSelection_.push_back(node);
        notifyChanged();
    }

    void addToSelection(DesignNode* node) {
        if (!node) return;
        if (std::find(multiSelection_.begin(), multiSelection_.end(), node)
            == multiSelection_.end()) {
            multiSelection_.push_back(node);
        }
        selected_ = node;
        notifyChanged();
    }

    void clearSelection() {
        selected_ = nullptr;
        multiSelection_.clear();
        notifyChanged();
    }

    const std::vector<DesignNode*>& multiSelection() const {
        return multiSelection_;
    }

    bool isSelected(DesignNode* node) const {
        return std::find(multiSelection_.begin(), multiSelection_.end(), node)
               != multiSelection_.end();
    }

    // ── Mutation (all go through notifyChanged) ────────────────────────

    /// Add a new child to `parent` (or root if parent is null).
    DesignNode* addChild(DesignNode* parent, const std::string& widgetType) {
        if (!parent) parent = root_.get();
        auto child = DesignNode::create(widgetType, nextId_++);
        child->parent = parent;
        auto* raw = child.get();
        parent->children.push_back(std::move(child));
        notifyChanged();
        return raw;
    }

    /// Remove a node from its parent.  Returns true on success.
    bool removeNode(DesignNode* node) {
        if (!node || node == root_.get()) return false;
        auto* par = node->parent;
        if (!par) return false;
        auto it = std::find_if(par->children.begin(), par->children.end(),
                               [&](const std::unique_ptr<DesignNode>& p) {
                                   return p.get() == node;
                               });
        if (it == par->children.end()) return false;

        // Clean up selection.
        for (auto sel = multiSelection_.begin();
             sel != multiSelection_.end(); ) {
            if (*sel == node) sel = multiSelection_.erase(sel);
            else ++sel;
        }
        if (selected_ == node) selected_ = nullptr;

        par->children.erase(it);
        notifyChanged();
        return true;
    }

    /// Move `node` to be a child of `newParent` at position `index`
    /// (-1 = append).  Returns true on success.
    bool reparent(DesignNode* node, DesignNode* newParent, int index = -1) {
        if (!node || !newParent || node == root_.get()) return false;
        if (node == newParent) return false;
        // Prevent reparenting to own descendant.
        for (auto* p = newParent; p; p = p->parent)
            if (p == node) return false;

        auto* oldParent = node->parent;
        if (!oldParent) return false;

        // Detach from old parent.
        auto it = std::find_if(oldParent->children.begin(),
                               oldParent->children.end(),
                               [&](const std::unique_ptr<DesignNode>& p) {
                                   return p.get() == node;
                               });
        if (it == oldParent->children.end()) return false;
        auto owned = std::move(*it);
        oldParent->children.erase(it);

        // Attach to new parent.
        owned->parent = newParent;
        if (index < 0 || index >= static_cast<int>(newParent->children.size()))
            newParent->children.push_back(std::move(owned));
        else
            newParent->children.insert(
                newParent->children.begin() + index, std::move(owned));

        notifyChanged();
        return true;
    }

    /// Set a property on a node and notify.
    void setProperty(DesignNode* node, const std::string& key,
                     const std::string& value) {
        if (!node) return;
        node->properties[key] = value;
        notifyChanged();
    }

    // ── Change notification ────────────────────────────────────────────
    void setOnChanged(std::function<void()> cb) { onChanged_ = std::move(cb); }
    void notifyChanged() { if (onChanged_) onChanged_(); }

    // ── Lookup helpers ─────────────────────────────────────────────────

    /// Find a node by its id (DFS).
    DesignNode* findById(const std::string& id) {
        return findByIdImpl(root_.get(), id);
    }

    /// Flatten all nodes into a vector (DFS order).
    std::vector<DesignNode*> flatten() {
        std::vector<DesignNode*> result;
        flattenImpl(root_.get(), result);
        return result;
    }

private:
    std::unique_ptr<DesignNode> root_;
    DesignNode* selected_ = nullptr;
    std::vector<DesignNode*> multiSelection_;
    std::function<void()> onChanged_;
    int nextId_ = 1;

    static DesignNode* findByIdImpl(DesignNode* node, const std::string& id) {
        if (!node) return nullptr;
        if (node->id == id) return node;
        for (auto& c : node->children)
            if (auto* r = findByIdImpl(c.get(), id)) return r;
        return nullptr;
    }

    static void flattenImpl(DesignNode* node,
                            std::vector<DesignNode*>& out) {
        if (!node) return;
        out.push_back(node);
        for (auto& c : node->children)
            flattenImpl(c.get(), out);
    }
};

}  // namespace designer
}  // namespace cui
