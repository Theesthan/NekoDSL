#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neko::ast {

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind { IntLiteral, FloatLiteral, Identifier, Call, BinaryOp, UnaryOp };

struct Expr {
    ExprKind    kind      = ExprKind::Identifier;
    int64_t     int_val   = 0;    // IntLiteral
    double      float_val = 0.0;  // FloatLiteral
    std::string name;             // Identifier / Call callee / BinaryOp op / UnaryOp op
    std::vector<Expr> args;       // Call args; BinaryOp: [lhs,rhs]; UnaryOp: [operand]

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
// Statements  (Block is defined below; use unique_ptr to break the cycle)
// ---------------------------------------------------------------------------

struct Block; // forward declaration — defined after Stmt

enum class StmtKind { Return, ExprStmt, VarDecl, Assign, If, While };

struct Stmt {
    StmtKind            kind       = StmtKind::ExprStmt;
    std::optional<Expr> expr;      // Return value / ExprStmt / VarDecl init / Assign rhs / If cond
    std::string         var_name;  // VarDecl name; Assign target
    std::string         var_type;  // VarDecl explicit type (empty = infer)
    bool                is_mutable = false; // VarDecl: true for `let mut`

    // If statement: then + optional else block (heap-allocated to break cycle).
    std::unique_ptr<Block> then_block;
    std::unique_ptr<Block> else_block; // null = no else

    // Non-copyable due to unique_ptr; movable.
    Stmt() = default;
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&&) = default;
    Stmt& operator=(Stmt&&) = default;

    static Stmt make_return(std::optional<Expr> e) {
        Stmt s; s.kind = StmtKind::Return; s.expr = std::move(e); return s;
    }
    static Stmt make_expr(Expr e) {
        Stmt s; s.kind = StmtKind::ExprStmt; s.expr = std::move(e); return s;
    }
    static Stmt make_var_decl(std::string name, std::string type, Expr init, bool mutable_) {
        Stmt s;
        s.kind       = StmtKind::VarDecl;
        s.var_name   = std::move(name);
        s.var_type   = std::move(type);
        s.expr       = std::move(init);
        s.is_mutable = mutable_;
        return s;
    }
    static Stmt make_assign(std::string target, Expr rhs) {
        Stmt s;
        s.kind     = StmtKind::Assign;
        s.var_name = std::move(target);
        s.expr     = std::move(rhs);
        return s;
    }
    static Stmt make_if(Expr cond, Block then_b, std::unique_ptr<Block> else_b);
    static Stmt make_while(Expr cond, Block body);
};

// ---------------------------------------------------------------------------
// Block (defined after Stmt so Stmt's unique_ptr<Block> members are usable)
// ---------------------------------------------------------------------------

struct Block {
    std::vector<Stmt> stmts;
};

// Defined out-of-line after Block is complete.
inline Stmt Stmt::make_if(Expr cond, Block then_b, std::unique_ptr<Block> else_b) {
    Stmt s;
    s.kind       = StmtKind::If;
    s.expr       = std::move(cond);
    s.then_block = std::make_unique<Block>(std::move(then_b));
    s.else_block = std::move(else_b);
    return s;
}

inline Stmt Stmt::make_while(Expr cond, Block body) {
    Stmt s;
    s.kind       = StmtKind::While;
    s.expr       = std::move(cond);
    s.then_block = std::make_unique<Block>(std::move(body));
    return s;
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

struct Param {
    std::string name;
    std::string type;
    std::string interp; // "", "flat", "noperspective", "centroid"
};

struct UniformField {
    std::string name;
    std::string type;
};

struct UniformBlock {
    std::string name;
    uint32_t    set     = 0;
    uint32_t    binding = 0;
    std::vector<UniformField> fields;
};

struct FunctionDecl {
    std::string name;
    std::string decorator;    // "vertex", "fragment", … or ""
    std::vector<Param> params;
    std::string return_type;  // "" means void
    std::string output_name;  // named output: "neko_position", "neko_color", etc.
    Block body;
};

// ---------------------------------------------------------------------------
// Translation unit
// ---------------------------------------------------------------------------

struct SamplerDecl {
    std::string name;
    uint32_t    set     = 0;
    uint32_t    binding = 0;
    std::string type;   // "sampler2D" (only type supported currently)
};

struct TranslationUnit {
    std::vector<FunctionDecl> functions;
    std::vector<UniformBlock> uniforms;
    std::vector<SamplerDecl>  samplers;
    std::vector<std::string>  imports;
};

} // namespace neko::ast
