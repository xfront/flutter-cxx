#pragma once
/// @file design_codegen.h
/// @brief Generates compilable CUI C++ source from a DesignModel tree.

#include <string>

namespace cui {
namespace designer {

class DesignModel;
struct DesignNode;

class CodeGenerator {
public:
    /// Generate a complete main.cpp from the model.
    std::string generate(const DesignModel& model) const;

private:
    void emitIncludes(const DesignModel& model, std::string& out) const;
    void emitNode(const DesignNode& node, const std::string& parentId,
                  std::string& out, int indent) const;
    void emitProperties(const DesignNode& node, const std::string& varName,
                        std::string& out) const;
    std::string varNameFor(const DesignNode& node) const;
    std::string indent(int level) const;
};

}  // namespace designer
}  // namespace cui
