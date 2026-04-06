#pragma once

#include "ast.hpp"
#include <string>

namespace neko {

/// Parse a NekoDSL source string and return the resulting AST.
/// Throws std::runtime_error on syntax errors.
ast::TranslationUnit parse_source(const std::string& source);

} // namespace neko
