#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neko::ast {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind { IntLiteral, FloatLiteral, Identifier, Call };

struct Expr {
    ExprKind    kind       = ExprKind::Identifier;
    int64_t     int_val    = 0;    // IntLiteral
    double      float_val  = 0.0;  // FloatLiteral
    std::string name;              // Identifier name / Call callee
    std::vector<Expr> args;        // Call arguments

    static Expr make_int(int64_t v) {
        return { ExprKind::IntLiteral, v, 0.0, {}, {} };
    }
    static Expr make_float(double v) {
        return { ExprKind::FloatLiteral, 0, v, {}, {} };
    }
    static Expr make_id(std::string n) {
        return { ExprKind::Identifier, 0, 0.0, std::move(n), {} };
    }
    static Expr make_call(std::string callee, std::vector<Expr> a) {
        return { ExprKind::Call, 0, 0.0, std::move(callee), std::move(a) };
    }
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind { Return, ExprStmt };

struct Stmt {
    StmtKind kind;
    std::optional<Expr> expr; // Return: optional value; ExprStmt: the expression

    static Stmt make_return(std::optional<Expr> e) {
        return { StmtKind::Return, std::move(e) };
    }
    static Stmt make_expr(Expr e) {
        return { StmtKind::ExprStmt, std::move(e) };
    }
};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

struct Block {
    std::vector<Stmt> stmts;
};

struct Param {
    std::string name;
    std::string type;
};

struct FunctionDecl {
    std::string name;
    std::string decorator;   // "vertex", "fragment", … or "" for none
    std::vector<Param> params;
    std::string return_type; // "" means void  (also the output variable type for entry points)
    std::string output_name; // named output: "neko_position", "neko_color", etc. ("" = none)
    Block body;
};

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

struct TranslationUnit {
    std::vector<FunctionDecl> functions;
};

} // namespace neko::ast
