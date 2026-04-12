#include "neko_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace neko {

// ============================================================================
// Token types
// ============================================================================

enum class TokKind {
    Ident,        // identifier
    IntLit,       // integer literal
    FloatLit,     // float literal (e.g. 1.0, 0.5f)
    BacktickStr,  // backtick-quoted string: `path/to/file`
    At,           // @
    LParen,       // (
    RParen,       // )
    LBrace,       // {
    RBrace,       // }
    Colon,        // :
    ColonEq,      // :=
    ColonColon,   // ::
    Dollar,       // $
    Comma,        // ,
    Semicolon,    // ;
    Eq,           // =
    EqEq,         // ==
    BangEq,       // !=
    Lt,           // <
    LtEq,         // <=
    Gt,           // >
    GtEq,         // >=
    Plus,         // +
    Minus,        // -
    Star,         // *
    Slash,        // /
    KwConst,      // const
    KwReturn,     // return
    KwLet,        // let
    KwMut,        // mut
    KwIf,         // if
    KwElse,       // else
    KwWhile,      // while
    KwImport,     // import
    KwExpose,     // expose
    DQuoteStr,    // "path/to/file"
    Eof
};

struct Token {
    TokKind     kind      = TokKind::Eof;
    std::string text;
    int64_t     int_val   = 0;
    double      float_val = 0.0;
};

// ============================================================================
// Lexer
// ============================================================================

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src), pos_(0) {}

    Token next() {
        skip_ws_and_comments();
        if (pos_ >= src_.size()) return {};

        const char c = src_[pos_];

        // ---- Backtick handling ----
        if (c == '`') {
            ++pos_;
            if (pos_ < src_.size()) {
                if (src_[pos_] == 'n') {
                    ++pos_; // `n == newline escape — skip and retry
                    return next();
                }
                // Backtick-quoted string literal
                std::string text;
                while (pos_ < src_.size() && src_[pos_] != '`' && src_[pos_] != '\n')
                    text += src_[pos_++];
                if (pos_ < src_.size() && src_[pos_] == '`') ++pos_; // consume closing `
                return { TokKind::BacktickStr, text };
            }
            return next(); // lone backtick at EOF
        }

        // ---- Identifier / keyword ----
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string text;
            while (pos_ < src_.size() &&
                   (std::isalnum(static_cast<unsigned char>(src_[pos_])) ||
                    src_[pos_] == '_')) {
                text += src_[pos_++];
            }
            if (text == "const")   return { TokKind::KwConst,  text };
            if (text == "return")  return { TokKind::KwReturn, text };
            if (text == "let")     return { TokKind::KwLet,    text };
            if (text == "mut")     return { TokKind::KwMut,    text };
            if (text == "if")      return { TokKind::KwIf,     text };
            if (text == "else")    return { TokKind::KwElse,   text };
            if (text == "while")   return { TokKind::KwWhile,  text };
            if (text == "import")  return { TokKind::KwImport, text };
            if (text == "expose")  return { TokKind::KwExpose, text };
            return { TokKind::Ident, text };
        }

        // ---- Numeric literals: integer or float ----
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string text;
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_])))
                text += src_[pos_++];

            // Float: has '.' followed by optional digits, optional 'f'/'F' suffix
            if (pos_ < src_.size() && src_[pos_] == '.') {
                text += '.';
                ++pos_;
                while (pos_ < src_.size() &&
                       std::isdigit(static_cast<unsigned char>(src_[pos_])))
                    text += src_[pos_++];
                if (pos_ < src_.size() &&
                    (src_[pos_] == 'f' || src_[pos_] == 'F'))
                    ++pos_; // consume 'f' suffix
                const double fv = std::stod(text);
                return { TokKind::FloatLit, text, 0, fv };
            }
            return { TokKind::IntLit, text, std::stoll(text) };
        }

        // ---- Double-quoted string literal (used in import paths) ----
        if (c == '"') {
            ++pos_;
            std::string text;
            while (pos_ < src_.size() && src_[pos_] != '"' && src_[pos_] != '\n')
                text += src_[pos_++];
            if (pos_ < src_.size() && src_[pos_] == '"') ++pos_; // closing "
            return { TokKind::DQuoteStr, text };
        }

        // ---- Punctuation ----
        ++pos_;
        switch (c) {
        case '@': return { TokKind::At };
        case '(': return { TokKind::LParen };
        case ')': return { TokKind::RParen };
        case '{': return { TokKind::LBrace };
        case '}': return { TokKind::RBrace };
        case '$': return { TokKind::Dollar };
        case ',': return { TokKind::Comma };
        case ';': return { TokKind::Semicolon };
        case '+': return { TokKind::Plus };
        case '-': return { TokKind::Minus };
        case '*': return { TokKind::Star };
        case '/': return { TokKind::Slash };
        case '=':
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::EqEq }; }
            return { TokKind::Eq };
        case '!':
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::BangEq }; }
            return next(); // lone '!' — skip
        case '<':
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::LtEq }; }
            return { TokKind::Lt };
        case '>':
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::GtEq }; }
            return { TokKind::Gt };
        case ':':
            if (pos_ < src_.size() && src_[pos_] == ':') { ++pos_; return { TokKind::ColonColon }; }
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::ColonEq }; }
            return { TokKind::Colon };
        default:
            return next(); // Unknown character: skip silently
        }
    }

    Token peek() {
        const size_t saved = pos_;
        Token t = next();
        pos_ = saved;
        return t;
    }

private:
    void skip_ws_and_comments() {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (std::isspace(static_cast<unsigned char>(c))) {
                ++pos_;
            } else if (pos_ + 1 < src_.size() && c == '/' && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else if (pos_ + 1 < src_.size() && c == '/' && src_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < src_.size() &&
                       !(src_[pos_] == '*' && src_[pos_ + 1] == '/'))
                    ++pos_;
                if (pos_ + 1 < src_.size()) pos_ += 2;
            } else {
                break;
            }
        }
    }

    const std::string& src_;
    size_t pos_;
};

// ============================================================================
// Recursive-descent Parser
// ============================================================================

namespace {

class NekoDSLParser {
public:
    explicit NekoDSLParser(const std::string& src) : lex_(src) {}

    ast::TranslationUnit parse() {
        ast::TranslationUnit tu;

        while (peek().kind != TokKind::Eof) {
            // Consume leading modifiers: private, inline, extern
            while (check_word("private") || check_word("inline") || check_word("extern"))
                consume();

            // expose [import(...)] — collect import paths or skip
            if (check(TokKind::KwExpose)) {
                consume();
                if (check(TokKind::KwImport)) {
                    for (auto& p : parse_import_paths())
                        tu.imports.push_back(std::move(p));
                    match(TokKind::Semicolon);
                } else {
                    skip_stmt();
                }
                continue;
            }

            // import(...) — collect paths
            if (check(TokKind::KwImport)) {
                for (auto& p : parse_import_paths())
                    tu.imports.push_back(std::move(p));
                match(TokKind::Semicolon);
                continue;
            }

            if (check(TokKind::At)) {
                consume(); // @
                const std::string deco = expect(TokKind::Ident).text;
                if (deco == "uniform") {
                    tu.uniforms.push_back(parse_uniform_block());
                } else if (deco == "sampler") {
                    tu.samplers.push_back(parse_sampler_decl());
                } else {
                    tu.functions.push_back(finish_function_decl(deco));
                }
                continue;
            }
            if (check(TokKind::KwConst)) {
                tu.functions.push_back(parse_const_decl());
                continue;
            }
            if (check(TokKind::Ident)) {
                Token name_tok = consume();
                if (check_word("operator")) { skip_stmt(); continue; }
                if (check(TokKind::ColonColon)) {
                    tu.functions.push_back(finish_colons_decl(name_tok.text));
                    continue;
                }
                skip_stmt();
                continue;
            }
            consume(); // skip unknown token
        }
        return tu;
    }

private:
    // ---- Token primitives ----
    Token consume()        { return lex_.next(); }
    Token peek()           { return lex_.peek(); }
    bool  check(TokKind k) { return peek().kind == k; }

    bool check_word(const char* w) {
        auto t = peek();
        return t.kind == TokKind::Ident && t.text == w;
    }

    bool match(TokKind k) {
        if (check(k)) { consume(); return true; }
        return false;
    }

    Token expect(TokKind k) {
        Token t = consume();
        if (t.kind != k)
            throw std::runtime_error(
                std::string("NekoDSL parse error: unexpected token '") + t.text + "'");
        return t;
    }

    // ---- Skip helpers ----
    void skip_stmt() {
        int depth = 0;
        while (!check(TokKind::Eof)) {
            if (check(TokKind::LBrace)) { ++depth; consume(); }
            else if (check(TokKind::RBrace)) {
                if (depth == 0) { consume(); return; }
                --depth; consume(); if (depth == 0) return;
            } else if (check(TokKind::Semicolon) && depth == 0) {
                consume(); return;
            } else consume();
        }
    }

    // ---- Import path extraction ----
    // Consumes: import ( "path" , "path" ... ) — caller handles trailing ';'
    std::vector<std::string> parse_import_paths() {
        consume(); // 'import'
        if (!match(TokKind::LParen)) { skip_stmt(); return {}; }
        std::vector<std::string> paths;
        while (!check(TokKind::RParen) && !check(TokKind::Eof)) {
            if (check(TokKind::DQuoteStr) || check(TokKind::BacktickStr))
                paths.push_back(consume().text);
            else
                consume(); // eat comma or unexpected token
        }
        match(TokKind::RParen);
        return paths;
    }

    // ---- @decorator + const decl  (decorator already consumed) ----
    ast::FunctionDecl finish_function_decl(const std::string& decorator) {
        ast::FunctionDecl decl;
        decl.decorator = decorator;
        expect(TokKind::KwConst);
        decl.name = expect(TokKind::Ident).text;
        expect(TokKind::ColonEq);
        expect(TokKind::LParen);
        parse_params(decl);
        expect(TokKind::RParen);
        parse_return_spec(decl);
        decl.body = parse_block();
        match(TokKind::Semicolon);
        return decl;
    }

    // ---- @uniform(set, binding) const name := { field : type, ... } ----
    ast::UniformBlock parse_uniform_block() {
        uint32_t set_val = 0, binding_val = 0;
        if (check(TokKind::LParen)) {
            consume();
            if (check(TokKind::IntLit)) set_val     = static_cast<uint32_t>(consume().int_val);
            match(TokKind::Comma);
            if (check(TokKind::IntLit)) binding_val = static_cast<uint32_t>(consume().int_val);
            expect(TokKind::RParen);
        }
        expect(TokKind::KwConst);
        ast::UniformBlock blk;
        blk.name    = expect(TokKind::Ident).text;
        blk.set     = set_val;
        blk.binding = binding_val;
        expect(TokKind::ColonEq);
        expect(TokKind::LBrace);
        while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
            ast::UniformField fld;
            fld.name = expect(TokKind::Ident).text;
            expect(TokKind::Colon);
            fld.type = expect(TokKind::Ident).text;
            blk.fields.push_back(std::move(fld));
            match(TokKind::Comma);
            match(TokKind::Semicolon);
        }
        expect(TokKind::RBrace);
        match(TokKind::Semicolon);
        return blk;
    }

    // ---- @sampler(set, binding) const name : sampler2D ; ----
    ast::SamplerDecl parse_sampler_decl() {
        uint32_t set_val = 0, binding_val = 0;
        if (check(TokKind::LParen)) {
            consume();
            if (check(TokKind::IntLit)) set_val     = static_cast<uint32_t>(consume().int_val);
            match(TokKind::Comma);
            if (check(TokKind::IntLit)) binding_val = static_cast<uint32_t>(consume().int_val);
            expect(TokKind::RParen);
        }
        expect(TokKind::KwConst);
        ast::SamplerDecl sd;
        sd.name    = expect(TokKind::Ident).text;
        sd.set     = set_val;
        sd.binding = binding_val;
        expect(TokKind::Colon);
        sd.type = expect(TokKind::Ident).text; // "sampler2D"
        match(TokKind::Semicolon);
        return sd;
    }

    // ---- const decl (no @ decorator) ----
    ast::FunctionDecl parse_const_decl() {
        ast::FunctionDecl decl;
        expect(TokKind::KwConst);
        decl.name = expect(TokKind::Ident).text;
        expect(TokKind::ColonEq);
        expect(TokKind::LParen);
        parse_params(decl);
        expect(TokKind::RParen);
        parse_return_spec(decl);
        decl.body = parse_block();
        match(TokKind::Semicolon);
        return decl;
    }

    // ---- name :: (params) $ output? { body }  (name already consumed) ----
    ast::FunctionDecl finish_colons_decl(std::string name) {
        ast::FunctionDecl decl;
        decl.name = std::move(name);
        if (decl.name.rfind("vertex", 0) == 0)   decl.decorator = "vertex";
        else if (decl.name.rfind("fragment", 0) == 0) decl.decorator = "fragment";
        expect(TokKind::ColonColon);
        expect(TokKind::LParen);
        parse_params(decl);
        expect(TokKind::RParen);
        parse_return_spec(decl);
        decl.body = parse_block();
        match(TokKind::Semicolon);
        return decl;
    }

    // ---- Shared helpers ----

    void parse_params(ast::FunctionDecl& decl) {
        if (check(TokKind::RParen)) return;
        do {
            ast::Param p;
            p.name = expect(TokKind::Ident).text;
            expect(TokKind::Colon);
            p.type = expect(TokKind::Ident).text;
            // Optional interpolation qualifier: @flat, @noperspective, @centroid
            if (check(TokKind::At)) {
                consume();
                p.interp = expect(TokKind::Ident).text;
            }
            decl.params.push_back(std::move(p));
        } while (match(TokKind::Comma));
    }

    // `$` followed by:
    //   `{`               → void
    //   `type`            → return_type only
    //   `name : type`     → output_name + return_type
    void parse_return_spec(ast::FunctionDecl& decl) {
        expect(TokKind::Dollar);
        if (!check(TokKind::Ident)) return; // void

        Token first = consume();
        if (check(TokKind::Colon)) {
            consume(); // ':'
            decl.output_name = first.text;
            if (check(TokKind::Ident)) decl.return_type = consume().text;
        } else {
            decl.return_type = first.text;
        }
    }

    // ---- Block & statement parsing ----

    ast::Block parse_block() {
        ast::Block block;
        expect(TokKind::LBrace);
        while (!check(TokKind::RBrace) && !check(TokKind::Eof))
            block.stmts.push_back(parse_stmt());
        expect(TokKind::RBrace);
        return block;
    }

    ast::Stmt parse_stmt() {
        // return statement
        if (check(TokKind::KwReturn)) {
            consume();
            if (check(TokKind::Semicolon)) { consume(); return ast::Stmt::make_return(std::nullopt); }
            ast::Expr e = parse_expr();
            expect(TokKind::Semicolon);
            return ast::Stmt::make_return(std::move(e));
        }

        // let binding: let [mut] name [: type] (= | :=) expr ;
        if (check(TokKind::KwLet)) {
            consume();
            const bool is_mut = match(TokKind::KwMut);
            std::string var_name = expect(TokKind::Ident).text;
            std::string var_type;
            if (check(TokKind::Colon)) {
                consume();
                var_type = expect(TokKind::Ident).text;
                expect(TokKind::Eq);
            } else if (check(TokKind::ColonEq)) {
                consume(); // := means inferred type
            } else {
                expect(TokKind::Eq); // bare = means inferred type
            }
            ast::Expr init = parse_expr();
            expect(TokKind::Semicolon);
            return ast::Stmt::make_var_decl(std::move(var_name), std::move(var_type),
                                            std::move(init), is_mut);
        }

        // if [else] statement
        if (check(TokKind::KwIf)) {
            consume();
            expect(TokKind::LParen);
            ast::Expr cond = parse_expr();
            expect(TokKind::RParen);
            ast::Block then_b = parse_block();
            std::unique_ptr<ast::Block> else_b;
            if (check(TokKind::KwElse)) {
                consume();
                else_b = std::make_unique<ast::Block>(parse_block());
            }
            return ast::Stmt::make_if(std::move(cond), std::move(then_b), std::move(else_b));
        }

        // while loop: while (cond) { body }
        if (check(TokKind::KwWhile)) {
            consume();
            expect(TokKind::LParen);
            ast::Expr cond = parse_expr();
            expect(TokKind::RParen);
            ast::Block body = parse_block();
            return ast::Stmt::make_while(std::move(cond), std::move(body));
        }

        // Skip SPIR-V intrinsics (__spirv_*, __spirv_type, etc.)
        if (check(TokKind::Ident) && peek().text.rfind("__", 0) == 0) {
            skip_stmt();
            return ast::Stmt::make_expr(ast::Expr::make_int(0)); // dummy
        }

        // Assignment: bare `ident = expr ;`
        // Distinguish from expr-stmt starting with an ident by peeking ahead.
        if (check(TokKind::Ident)) {
            const std::string name = peek().text;
            // Use a two-token look-ahead: consume the ident, check for '='.
            // If it's not '=', put it back by parsing it as an expression.
            // We don't have undo, so instead we build a primary expr and check
            // whether the next token after it is '=' (which would be the
            // assignment operator, not a comparison — NekoDSL has no '==').
            // Since parse_primary will consume the ident and possibly a call,
            // we only detect `plain_ident = expr` here; complex lhs is future work.
            const Token saved_peek = peek(); // peek is non-destructive
            consume(); // consume the ident
            if (check(TokKind::Eq)) {
                consume(); // consume '='
                ast::Expr rhs = parse_expr();
                expect(TokKind::Semicolon);
                return ast::Stmt::make_assign(name, std::move(rhs));
            }
            // Not an assignment — reconstruct as expression statement.
            // The ident is already consumed; build a primary, then let
            // parse_comparison_rhs finish any operators that follow.
            ast::Expr lhs = ast::Expr::make_id(name);
            if (check(TokKind::LParen)) {
                consume();
                std::vector<ast::Expr> args;
                if (!check(TokKind::RParen)) {
                    do { args.push_back(parse_expr()); } while (match(TokKind::Comma));
                }
                expect(TokKind::RParen);
                lhs = ast::Expr::make_call(name, std::move(args));
            }
            // Continue with any binary operators (mul, add, compare)
            lhs = finish_mul(std::move(lhs));
            lhs = finish_add(std::move(lhs));
            lhs = finish_cmp(std::move(lhs));
            expect(TokKind::Semicolon);
            return ast::Stmt::make_expr(std::move(lhs));
        }

        ast::Expr e = parse_expr();
        expect(TokKind::Semicolon);
        return ast::Stmt::make_expr(std::move(e));
    }

    // ---- Expression parsing (operator precedence) ----
    //
    // Grammar (lowest to highest precedence):
    //   expr           → comparison
    //   comparison     → additive (('==' | '!=' | '<' | '<=' | '>' | '>=') additive)*
    //   additive       → multiplicative (('+' | '-') multiplicative)*
    //   multiplicative → unary (('*' | '/') unary)*
    //   unary          → '-' unary | primary
    //   primary        → '(' expr ')' | FloatLit | IntLit | Ident ('(' args ')')?

    ast::Expr parse_expr()           { return parse_comparison(); }

    bool is_comparison_op() {
        const TokKind k = peek().kind;
        return k == TokKind::EqEq  || k == TokKind::BangEq ||
               k == TokKind::Lt    || k == TokKind::LtEq   ||
               k == TokKind::Gt    || k == TokKind::GtEq;
    }

    ast::Expr parse_comparison() {
        ast::Expr lhs = parse_additive();
        while (is_comparison_op()) {
            const TokKind op_kind = consume().kind;
            std::string op;
            switch (op_kind) {
            case TokKind::EqEq:  op = "=="; break;
            case TokKind::BangEq: op = "!="; break;
            case TokKind::Lt:    op = "<";  break;
            case TokKind::LtEq:  op = "<="; break;
            case TokKind::Gt:    op = ">";  break;
            default:             op = ">="; break;
            }
            lhs = ast::Expr::make_binop(std::move(op), std::move(lhs), parse_additive());
        }
        return lhs;
    }

    // Continuation helpers — take an already-parsed lhs and apply remaining operators.
    ast::Expr finish_mul(ast::Expr lhs) {
        while (check(TokKind::Star) || check(TokKind::Slash)) {
            const TokKind op_kind = consume().kind;
            const std::string op  = (op_kind == TokKind::Star) ? "*" : "/";
            lhs = ast::Expr::make_binop(op, std::move(lhs), parse_unary());
        }
        return lhs;
    }

    ast::Expr finish_add(ast::Expr lhs) {
        while (check(TokKind::Plus) || check(TokKind::Minus)) {
            const TokKind op_kind = consume().kind;
            const std::string op  = (op_kind == TokKind::Plus) ? "+" : "-";
            lhs = ast::Expr::make_binop(op, std::move(lhs), parse_multiplicative());
        }
        return lhs;
    }

    ast::Expr finish_cmp(ast::Expr lhs) {
        while (is_comparison_op()) {
            const TokKind op_kind = consume().kind;
            std::string op;
            switch (op_kind) {
            case TokKind::EqEq:   op = "=="; break;
            case TokKind::BangEq: op = "!="; break;
            case TokKind::Lt:     op = "<";  break;
            case TokKind::LtEq:   op = "<="; break;
            case TokKind::Gt:     op = ">";  break;
            default:              op = ">="; break;
            }
            lhs = ast::Expr::make_binop(std::move(op), std::move(lhs), parse_additive());
        }
        return lhs;
    }

    ast::Expr parse_additive() {
        return finish_add(parse_multiplicative());
    }

    ast::Expr parse_multiplicative() {
        return finish_mul(parse_unary());
    }

    ast::Expr parse_unary() {
        if (check(TokKind::Minus)) {
            consume();
            return ast::Expr::make_unop("-", parse_unary());
        }
        return parse_primary();
    }

    ast::Expr parse_primary() {
        // Parenthesized expression
        if (check(TokKind::LParen)) {
            consume();
            ast::Expr e = parse_expr();
            expect(TokKind::RParen);
            return e;
        }
        if (check(TokKind::FloatLit)) {
            Token t = consume();
            return ast::Expr::make_float(t.float_val);
        }
        if (check(TokKind::IntLit)) {
            Token t = consume();
            return ast::Expr::make_int(t.int_val);
        }
        if (check(TokKind::Ident)) {
            std::string name = consume().text;
            if (check(TokKind::LParen)) {
                consume();
                std::vector<ast::Expr> args;
                if (!check(TokKind::RParen)) {
                    do { args.push_back(parse_expr()); } while (match(TokKind::Comma));
                }
                expect(TokKind::RParen);
                return ast::Expr::make_call(std::move(name), std::move(args));
            }
            return ast::Expr::make_id(std::move(name));
        }
        throw std::runtime_error("NekoDSL parse error: expected expression");
    }

    Lexer lex_;
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

ast::TranslationUnit parse_source(const std::string& source) {
    NekoDSLParser p(source);
    return p.parse();
}

} // namespace neko
