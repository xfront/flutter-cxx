#pragma once
/// @file design_codeparser.h
/// @brief Simplified C++ parser — reconstructs a DesignModel from generated
///        CUI source code.  Recognises common patterns:
///          auto <var> = cui::make_shared<<Type>>(<args>);
///          <var>->setText("...");
///          <parent>->addChild(<var>);

#include <string>
#include <vector>
#include "cui/foundation/shared_ptr.h"

namespace cui {
namespace designer {

class DesignModel;

class CodeParser {
public:
    struct ParseError {
        int line;
        std::string message;
    };

    /// Parse C++ source and rebuild the model.  Returns true on success.
    /// On failure, populates `errors` and leaves the model unchanged.
    bool parse(const std::string& source, DesignModel& model,
               std::vector<ParseError>& errors) const;
};

}  // namespace designer
}  // namespace cui
