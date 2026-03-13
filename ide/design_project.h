#pragma once
/// @file design_project.h
/// @brief Project management — save/load designs, browse filesystem,
///        export code, open source files.
///
/// ProjectManager handles:
///   - Serializing DesignModel to/from a JSON-like text format
///   - Opening a project directory and browsing its files
///   - Saving/loading .cuidesign project files
///   - Exporting generated C++ code to disk
///   - Loading source files into the code editor

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cui {
namespace designer {

class DesignModel;
struct DesignNode;

/// Filesystem entry for the Solution Explorer.
struct FileEntry {
    std::string name;
    std::string path;         // full path
    bool isDir = false;
    std::vector<std::unique_ptr<FileEntry>> children;
};

/// Manages project-level operations for the designer.
class ProjectManager {
public:
    ProjectManager();

    // ── Project lifecycle ──────────────────────────────────────────────

    /// Create a new empty project in the given directory.
    /// Creates the directory if it doesn't exist.
    bool newProject(const std::string& directory);

    /// Open an existing project directory.  Looks for a .cuidesign file.
    /// Returns true if a project file was found and loaded.
    bool openProject(const std::string& directory);

    /// Save the current design model to the project file.
    bool saveProject();

    /// Save the design model to a specific file path.
    bool saveProjectAs(const std::string& path);

    /// Export generated C++ code to a file.
    bool exportCode(const std::string& code, const std::string& filePath);

    // ── Filesystem browsing ────────────────────────────────────────────

    /// Build a filesystem tree for the given directory (max depth).
    std::unique_ptr<FileEntry> browseDirectory(const std::string& path,
                                                int maxDepth = 3);

    /// Read a text file's contents.  Returns empty string on failure.
    std::string readFile(const std::string& path);

    /// Write text to a file.  Returns true on success.
    bool writeFile(const std::string& path, const std::string& content);

    // ── Model serialization ────────────────────────────────────────────

    /// Serialize the design model to a JSON string.
    std::string serializeModel(const DesignModel& model) const;

    /// Deserialize a JSON string into the design model.
    /// Returns true on success.
    bool deserializeModel(const std::string& json, DesignModel& model);

    // ── State accessors ────────────────────────────────────────────────

    bool hasProject() const { return !projectDir_.empty(); }
    const std::string& projectDir() const { return projectDir_; }
    const std::string& projectFile() const { return projectFile_; }
    const std::string& projectName() const { return projectName_; }

    /// Set the model to operate on.
    void setModel(DesignModel* model) { model_ = model; }

private:
    DesignModel* model_ = nullptr;
    std::string projectDir_;
    std::string projectFile_;
    std::string projectName_;

    // Serialization helpers.
    void serializeNode(const DesignNode& node, std::string& out,
                       int indent) const;
    bool parseJsonObject(const std::string& json, size_t& pos,
                         DesignNode& node);
    std::string parseString(const std::string& json, size_t& pos);
    void skipWhitespace(const std::string& json, size_t& pos);

    static std::string escapeJson(const std::string& s);
    static std::string unescapeJson(const std::string& s);
    static std::string indent(int level);
};

}  // namespace designer
}  // namespace cui
