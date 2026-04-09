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
    Plus,         // +
    Minus,        // -
    Star,         // *
    Slash,        // /
    KwConst,      // const
    KwReturn,     // return
    KwLet,        // let
    KwImport,     // import
    KwExpose,     // expose
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
        case '=': return { TokKind::Eq };
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
            // Skip module-level directives
            if (check(TokKind::KwImport) || check(TokKind::KwExpose)) {
                skip_stmt();
                continue;
            }
            // Skip modifier prefixes
            while (check_word("inline") || check_word("private") || check_word("extern"))
                consume();

            if (check(TokKind::At)) {
                tu.functions.push_back(parse_at_const_decl());
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

    // ---- @decorator + const decl ----
    ast::FunctionDecl parse_at_const_decl() {
        ast::FunctionDecl decl;
        expect(TokKind::At);
        decl.decorator = expect(TokKind::Ident).text;
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

        // let binding: let name : type = expr ; OR let name = expr ;
        if (check(TokKind::KwLet)) {
            consume();
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
            return ast::Stmt::make_var_decl(std::move(var_name), std::move(var_type), std::move(init));
        }

        // Skip SPIR-V intrinsics (__spirv_*, __spirv_type, etc.)
        if (check(TokKind::Ident) && peek().text.rfind("__", 0) == 0) {
            skip_stmt();
            return ast::Stmt::make_expr(ast::Expr::make_int(0)); // dummy
        }

        ast::Expr e = parse_expr();
        expect(TokKind::Semicolon);
        return ast::Stmt::make_expr(std::move(e));
    }

    // ---- Expression parsing (operator precedence) ----
    //
    // Grammar (lowest to highest precedence):
    //   expr        → additive
    //   additive    → multiplicative (('+' | '-') multiplicative)*
    //   multiplicative → unary (('*' | '/') unary)*
    //   unary       → '-' unary | primary
    //   primary     → '(' expr ')' | FloatLit | IntLit | Ident ('(' args ')')?

    ast::Expr parse_expr()           { return parse_additive(); }

    ast::Expr parse_additive() {
        ast::Expr lhs = parse_multiplicative();
        while (check(TokKind::Plus) || check(TokKind::Minus)) {
            const TokKind op_kind = consume().kind;
            const std::string op  = (op_kind == TokKind::Plus) ? "+" : "-";
            lhs = ast::Expr::make_binop(op, std::move(lhs), parse_multiplicative());
        }
        return lhs;
    }

    ast::Expr parse_multiplicative() {
        ast::Expr lhs = parse_unary();
        while (check(TokKind::Star) || check(TokKind::Slash)) {
            const TokKind op_kind = consume().kind;
            const std::string op  = (op_kind == TokKind::Star) ? "*" : "/";
            lhs = ast::Expr::make_binop(op, std::move(lhs), parse_unary());
        }
        return lhs;
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
