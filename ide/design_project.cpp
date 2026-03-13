/// @file design_project.cpp
/// @brief ProjectManager implementation — save/load, filesystem, code export.

#include "design_project.h"
#include "design_model.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cui {
namespace designer {

// ── Constructor ────────────────────────────────────────────────────────────

ProjectManager::ProjectManager() = default;

// ── JSON helpers ───────────────────────────────────────────────────────────

std::string ProjectManager::escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string ProjectManager::unescapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '"':  out += '"';  ++i; break;
                case '\\': out += '\\'; ++i; break;
                case 'n':  out += '\n'; ++i; break;
                case 'r':  out += '\r'; ++i; break;
                case 't':  out += '\t'; ++i; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string ProjectManager::indent(int level) {
    return std::string(level * 2, ' ');
}

void ProjectManager::skipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' ||
            json[pos] == '\r' || json[pos] == '\t'))
        ++pos;
}

std::string ProjectManager::parseString(const std::string& json,
                                         size_t& pos) {
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;  // skip opening quote
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            result += json[pos];
            result += json[pos + 1];
            pos += 2;
        } else {
            result += json[pos];
            ++pos;
        }
    }
    if (pos < json.size()) ++pos;  // skip closing quote
    return unescapeJson(result);
}

// ── Serialization ──────────────────────────────────────────────────────────

void ProjectManager::serializeNode(const DesignNode& node, std::string& out,
                                    int ind) const {
    out += indent(ind) + "{\n";
    out += indent(ind + 1) + "\"id\": \"" + escapeJson(node.id) + "\",\n";
    out += indent(ind + 1) + "\"type\": \"" + escapeJson(node.widgetType) + "\",\n";
    out += indent(ind + 1) + "\"name\": \"" + escapeJson(node.displayName) + "\",\n";

    // Properties.
    out += indent(ind + 1) + "\"props\": {";
    bool first = true;
    for (auto& [k, v] : node.properties) {
        if (!first) out += ",";
        out += "\n" + indent(ind + 2) + "\"" + escapeJson(k) + "\": \""
             + escapeJson(v) + "\"";
        first = false;
    }
    out += "\n" + indent(ind + 1) + "},\n";

    // Children.
    out += indent(ind + 1) + "\"children\": [";
    if (node.children.empty()) {
        out += "]";
    } else {
        out += "\n";
        for (size_t i = 0; i < node.children.size(); ++i) {
            serializeNode(*node.children[i], out, ind + 2);
            if (i + 1 < node.children.size()) out += ",";
            out += "\n";
        }
        out += indent(ind + 1) + "]";
    }

    out += "\n" + indent(ind) + "}";
}

std::string ProjectManager::serializeModel(const DesignModel& model) const {
    std::string out;
    out += "{\n";
    out += "  \"version\": 1,\n";

    // Root node.
    out += "  \"root\": ";
    if (model.root()) {
        serializeNode(*model.root(), out, 2);
    } else {
        out += "null";
    }
    out += "\n}\n";
    return out;
}

// ── Deserialization ────────────────────────────────────────────────────────

bool ProjectManager::parseJsonObject(const std::string& json, size_t& pos,
                                      DesignNode& node) {
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    ++pos;  // skip '{'

    while (pos < json.size() && json[pos] != '}') {
        skipWhitespace(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] == ',') { ++pos; continue; }

        // Parse key.
        std::string key = parseString(json, pos);
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ':') ++pos;
        skipWhitespace(json, pos);

        if (key == "id") {
            node.id = parseString(json, pos);
        } else if (key == "type") {
            node.widgetType = parseString(json, pos);
        } else if (key == "name") {
            node.displayName = parseString(json, pos);
        } else if (key == "props") {
            // Parse props object.
            if (pos < json.size() && json[pos] == '{') {
                ++pos;
                while (pos < json.size() && json[pos] != '}') {
                    skipWhitespace(json, pos);
                    if (pos < json.size() && json[pos] == ',') { ++pos; continue; }
                    std::string pk = parseString(json, pos);
                    skipWhitespace(json, pos);
                    if (pos < json.size() && json[pos] == ':') ++pos;
                    std::string pv = parseString(json, pos);
                    if (!pk.empty())
                        node.properties[pk] = pv;
                }
                if (pos < json.size()) ++pos;  // skip '}'
            }
        } else if (key == "children") {
            // Parse children array.
            if (pos < json.size() && json[pos] == '[') {
                ++pos;
                while (pos < json.size() && json[pos] != ']') {
                    skipWhitespace(json, pos);
                    if (pos < json.size() && json[pos] == ',') { ++pos; continue; }
                    if (pos < json.size() && json[pos] == '{') {
                        auto child = std::make_unique<DesignNode>();
                        child->parent = &node;
                        if (parseJsonObject(json, pos, *child)) {
                            node.children.push_back(std::move(child));
                        }
                    } else {
                        ++pos;  // skip unexpected char
                    }
                }
                if (pos < json.size()) ++pos;  // skip ']'
            }
        } else {
            // Skip unknown value (string, number, object, array).
            if (json[pos] == '"') {
                parseString(json, pos);
            } else if (json[pos] == '{') {
                int depth = 1;
                ++pos;
                while (pos < json.size() && depth > 0) {
                    if (json[pos] == '{') ++depth;
                    else if (json[pos] == '}') --depth;
                    ++pos;
                }
            } else if (json[pos] == '[') {
                int depth = 1;
                ++pos;
                while (pos < json.size() && depth > 0) {
                    if (json[pos] == '[') ++depth;
                    else if (json[pos] == ']') --depth;
                    ++pos;
                }
            } else {
                // Number or literal — skip to comma or closing brace.
                while (pos < json.size() && json[pos] != ',' &&
                       json[pos] != '}' && json[pos] != ']')
                    ++pos;
            }
        }
    }

    if (pos < json.size()) ++pos;  // skip '}'
    return true;
}

bool ProjectManager::deserializeModel(const std::string& json,
                                       DesignModel& model) {
    // Find "root" key.
    size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    ++pos;

    while (pos < json.size() && json[pos] != '}') {
        skipWhitespace(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] == ',') { ++pos; continue; }

        std::string key = parseString(json, pos);
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ':') ++pos;
        skipWhitespace(json, pos);

        if (key == "root" && json[pos] == '{') {
            // Clear existing model and parse root node.
            // Remove all children from root.
            while (!model.root()->children.empty())
                model.removeNode(model.root()->children.back().get());

            DesignNode parsed;
            if (parseJsonObject(json, pos, parsed)) {
                // Apply parsed root properties.
                model.root()->id = parsed.id;
                model.root()->widgetType = parsed.widgetType;
                model.root()->displayName = parsed.displayName;
                model.root()->properties = parsed.properties;

                // Move children into the model.
                for (auto& child : parsed.children) {
                    child->parent = model.root();
                    model.root()->children.push_back(std::move(child));
                }
                model.notifyChanged();
                return true;
            }
        } else {
            // Skip value.
            if (pos < json.size() && json[pos] == '"') {
                parseString(json, pos);
            } else if (pos < json.size() && json[pos] == '{') {
                int d = 1; ++pos;
                while (pos < json.size() && d > 0) {
                    if (json[pos] == '{') ++d;
                    else if (json[pos] == '}') --d;
                    ++pos;
                }
            } else if (pos < json.size() && json[pos] == '[') {
                int d = 1; ++pos;
                while (pos < json.size() && d > 0) {
                    if (json[pos] == '[') ++d;
                    else if (json[pos] == ']') --d;
                    ++pos;
                }
            } else {
                while (pos < json.size() && json[pos] != ',' &&
                       json[pos] != '}')
                    ++pos;
            }
        }
    }
    return false;
}

// ── Project lifecycle ──────────────────────────────────────────────────────

bool ProjectManager::newProject(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        std::fprintf(stderr, "[Project] Failed to create directory: %s\n",
                     ec.message().c_str());
        return false;
    }

    projectDir_ = fs::canonical(directory, ec).string();
    if (projectDir_.empty()) projectDir_ = directory;

    projectName_ = fs::path(projectDir_).filename().string();
    projectFile_ = (fs::path(projectDir_) / (projectName_ + ".cuidesign"))
                       .string();

    // Write initial project file.
    if (model_) {
        std::string json = serializeModel(*model_);
        writeFile(projectFile_, json);
    }

    std::fprintf(stderr, "[Project] New project: %s\n", projectDir_.c_str());
    return true;
}

bool ProjectManager::openProject(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        std::fprintf(stderr, "[Project] Not a directory: %s\n",
                     directory.c_str());
        return false;
    }

    projectDir_ = fs::canonical(directory, ec).string();
    if (projectDir_.empty()) projectDir_ = directory;
    projectName_ = fs::path(projectDir_).filename().string();

    // Look for .cuidesign file.
    projectFile_.clear();
    for (auto& entry : fs::directory_iterator(projectDir_, ec)) {
        if (entry.path().extension() == ".cuidesign") {
            projectFile_ = entry.path().string();
            break;
        }
    }

    if (!projectFile_.empty() && model_) {
        std::string json = readFile(projectFile_);
        if (!json.empty()) {
            if (deserializeModel(json, *model_)) {
                std::fprintf(stderr, "[Project] Loaded: %s\n",
                             projectFile_.c_str());
                return true;
            }
        }
    }

    std::fprintf(stderr, "[Project] Opened directory: %s (no design file)\n",
                 projectDir_.c_str());
    return false;
}

bool ProjectManager::saveProject() {
    if (projectFile_.empty() || !model_) return false;
    std::string json = serializeModel(*model_);
    if (writeFile(projectFile_, json)) {
        std::fprintf(stderr, "[Project] Saved: %s\n", projectFile_.c_str());
        return true;
    }
    return false;
}

bool ProjectManager::saveProjectAs(const std::string& path) {
    projectFile_ = path;
    return saveProject();
}

bool ProjectManager::exportCode(const std::string& code,
                                 const std::string& filePath) {
    if (writeFile(filePath, code)) {
        std::fprintf(stderr, "[Project] Exported code: %s (%zu bytes)\n",
                     filePath.c_str(), code.size());
        return true;
    }
    return false;
}

// ── Filesystem browsing ────────────────────────────────────────────────────

std::unique_ptr<FileEntry> ProjectManager::browseDirectory(
    const std::string& path, int maxDepth) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto entry = std::make_unique<FileEntry>();
    fs::path p(path);
    entry->name = p.filename().string();
    if (entry->name.empty()) entry->name = path;
    entry->path = fs::canonical(p, ec).string();
    if (entry->path.empty()) entry->path = path;
    entry->isDir = fs::is_directory(p, ec);

    if (!entry->isDir || maxDepth <= 0) return entry;

    // Sort: directories first, then files, alphabetically.
    std::vector<fs::directory_entry> entries;
    for (auto& e : fs::directory_iterator(p, ec))
        entries.push_back(e);

    std::sort(entries.begin(), entries.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  bool ad = a.is_directory();
                  bool bd = b.is_directory();
                  if (ad != bd) return ad > bd;  // dirs first
                  return a.path().filename() < b.path().filename();
              });

    // Skip hidden files and common build directories.
    for (auto& e : entries) {
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (name == "build" || name == "node_modules" || name == "__pycache__")
            continue;

        auto child = std::make_unique<FileEntry>();
        child->name = name;
        child->path = e.path().string();
        child->isDir = e.is_directory();

        if (child->isDir && maxDepth > 1) {
            // Recurse into subdirectory.
            auto sub = browseDirectory(child->path, maxDepth - 1);
            child->children = std::move(sub->children);
        }

        entry->children.push_back(std::move(child));
    }

    return entry;
}

// ── File I/O ───────────────────────────────────────────────────────────────

std::string ProjectManager::readFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

bool ProjectManager::writeFile(const std::string& path,
                                const std::string& content) {
    // Create parent directories if needed.
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);

    std::ofstream ofs(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}

}  // namespace designer
}  // namespace cui_core
