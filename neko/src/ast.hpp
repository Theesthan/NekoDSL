#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neko::ast {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind { IntLiteral, FloatLiteral, Identifier, Call, BinaryOp, UnaryOp };

struct Expr {
    ExprKind    kind       = ExprKind::Identifier;
    int64_t     int_val    = 0;    // IntLiteral
    double      float_val  = 0.0;  // FloatLiteral
    std::string name;              // Identifier name / Call callee / BinaryOp op / UnaryOp op
    std::vector<Expr> args;        // Call args; BinaryOp: [lhs, rhs]; UnaryOp: [operand]

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
    static Expr make_binop(std::string op, Expr lhs, Expr rhs) {
        std::vector<Expr> args;
        args.push_back(std::move(lhs));
        args.push_back(std::move(rhs));
        return { ExprKind::BinaryOp, 0, 0.0, std::move(op), std::move(args) };
    }
    static Expr make_unop(std::string op, Expr operand) {
        std::vector<Expr> args;
        args.push_back(std::move(operand));
        return { ExprKind::UnaryOp, 0, 0.0, std::move(op), std::move(args) };
    }
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind { Return, ExprStmt, VarDecl };

struct Stmt {
    StmtKind kind      = StmtKind::ExprStmt;
    std::optional<Expr> expr; // Return: optional value; ExprStmt/VarDecl: the expression
    std::string var_name;     // VarDecl: variable name
    std::string var_type;     // VarDecl: explicit type (empty = infer from expr)

    static Stmt make_return(std::optional<Expr> e) {
        return { StmtKind::Return, std::move(e) };
    }
    static Stmt make_expr(Expr e) {
        return { StmtKind::ExprStmt, std::move(e) };
    }
    static Stmt make_var_decl(std::string name, std::string type, Expr init) {
        Stmt s;
        s.kind     = StmtKind::VarDecl;
        s.var_name = std::move(name);
        s.var_type = std::move(type);
        s.expr     = std::move(init);
        return s;
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
    std::string return_type; // "" means void
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
