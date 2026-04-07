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
    KwConst,      // const
    KwReturn,     // return
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
        // Backtick escapes: `n = newline (skip), `X = skip backtick only.
        // Backtick-quoted string: `content` → BacktickStr token.
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

        // ---- Multi-char punctuation ----
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
        case ':':
            if (pos_ < src_.size() && src_[pos_] == ':') { ++pos_; return { TokKind::ColonColon }; }
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::ColonEq }; }
            return { TokKind::Colon };
        default:
            // Unknown character: skip silently and try again
            return next();
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

class Parser {
public:
    explicit Parser(const std::string& src) : lex_(src) {}

    ast::TranslationUnit parse() {
        ast::TranslationUnit tu;
        while (peek().kind != TokKind::Eof) {
            // Skip module-system keywords that we don't compile
            if (check(TokKind::KwImport) || check(TokKind::KwExpose)) {
                skip_to_semicolon();
                continue;
            }
            // Skip modifier keywords that precede a declaration
            if (check_word("inline") || check_word("private") || check_word("extern"))
                consume();
            // Skip 'operator' keyword (operators.neko style — not yet supported)
            if (check_word("operator")) {
                skip_to_brace_end();
                continue;
            }

            if (check(TokKind::KwConst)) {
                tu.functions.push_back(parse_const_decl());
            } else if (check(TokKind::Ident)) {
                // Could be a `name :: (...)` declaration or junk — peek ahead
                Token name_tok = peek();
                if (is_colons_decl()) {
                    tu.functions.push_back(parse_colons_decl());
                } else {
                    consume(); // skip unknown ident
                }
            } else {
                consume(); // skip unknown token
            }
        }
        return tu;
    }

private:
    // ---- Token helpers ----

    Token consume()                   { return lex_.next(); }
    Token peek()                      { return lex_.peek(); }
    bool  check(TokKind k)            { return peek().kind == k; }

    bool check_word(const char* word) {
        auto t = peek();
        return t.kind == TokKind::Ident && t.text == word;
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

    void skip_to_semicolon() {
        int depth = 0;
        while (!check(TokKind::Eof)) {
            TokKind k = peek().kind;
            if (k == TokKind::LBrace) { ++depth; consume(); }
            else if (k == TokKind::RBrace) {
                if (depth == 0) { consume(); return; }
                --depth; consume();
            } else if (k == TokKind::Semicolon && depth == 0) {
                consume(); return;
            } else {
                consume();
            }
        }
    }

    void skip_to_brace_end() {
        // Skip until we consume the matching '}'
        while (!check(TokKind::LBrace) && !check(TokKind::Eof)) consume();
        if (check(TokKind::LBrace)) {
            int depth = 0;
            while (!check(TokKind::Eof)) {
                if (check(TokKind::LBrace))       { ++depth;  consume(); }
                else if (check(TokKind::RBrace))  { --depth;  consume(); if (depth == 0) return; }
                else consume();
            }
        }
    }

    // ---- Lookahead: is the next declaration a `name :: (...)` form? ----

    bool is_colons_decl() {
        // Peek 2 tokens: Ident followed by ColonColon
        const size_t saved = lex_pos();
        consume(); // ident
        bool ok = check(TokKind::ColonColon);
        restore_pos(saved);
        return ok;
    }

    // ---- Public API helpers to save/restore lexer position ----
    // (used only by is_colons_decl)

    size_t lex_pos() {
        // Hack: peek() already saves/restores internally, but we need to save
        // the position BEFORE the first consume() inside is_colons_decl.
        // We use two-token lookahead via two consecutive peek()+consume() calls.
        // Since peek() restores position automatically, we just call next() twice
        // and restore via a wrapper — but we don't have direct access to lexer pos.
        // Instead, use a different strategy: in is_colons_decl we call consume()
        // and then check. But peek() doesn't consume, so we can just peek ahead.
        return 0; // unused — see is_colons_decl_v2 below
    }
    void restore_pos(size_t) {} // unused

    // ---- `const name := (params) $ out? { body }` ----

    ast::FunctionDecl parse_const_decl() {
        ast::FunctionDecl decl;
        expect(TokKind::KwConst);
        decl.name = expect(TokKind::Ident).text;
        expect(TokKind::ColonEq);
        expect(TokKind::LParen);
        parse_param_list(decl);
        expect(TokKind::RParen);
        parse_return_spec(decl);
        decl.body = parse_block();
        match(TokKind::Semicolon);
        return decl;
    }

    // ---- `name :: (params) $ out? { body }` ----

    ast::FunctionDecl parse_colons_decl() {
        ast::FunctionDecl decl;
        decl.name = expect(TokKind::Ident).text;

        // Infer execution model from function name prefix
        if (decl.name.rfind("vertex", 0) == 0)
            decl.decorator = "vertex";
        else if (decl.name.rfind("fragment", 0) == 0)
            decl.decorator = "fragment";

        expect(TokKind::ColonColon);
        expect(TokKind::LParen);
        parse_param_list(decl);
        expect(TokKind::RParen);
        parse_return_spec(decl);
        decl.body = parse_block();
        match(TokKind::Semicolon);
        return decl;
    }

    // ---- Shared helpers ----

    void parse_param_list(ast::FunctionDecl& decl) {
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
    //   nothing / `{`              → void
    //   `type`                     → return_type = type
    //   `out_name : type`          → output_name = out_name, return_type = type
    void parse_return_spec(ast::FunctionDecl& decl) {
        expect(TokKind::Dollar);
        if (!check(TokKind::Ident)) return; // void — nothing follows $ before {

        Token first = consume();
        if (check(TokKind::Colon)) {
            // Named output: first is output_name, next is output type
            consume(); // ':'
            decl.output_name  = first.text;
            if (check(TokKind::Ident))
                decl.return_type = consume().text;
        } else {
            // Plain return type
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
        if (check(TokKind::KwReturn)) {
            consume();
            if (check(TokKind::Semicolon)) { consume(); return ast::Stmt::make_return(std::nullopt); }
            ast::Expr e = parse_expr();
            expect(TokKind::Semicolon);
            return ast::Stmt::make_return(std::move(e));
        }
        // Skip __spirv/__spirv_glsl/__spirv_type intrinsics as unknown statements
        if (check(TokKind::Ident) && peek().text.rfind("__", 0) == 0) {
            skip_to_semicolon();
            // Return a dummy expr statement that evaluates to int 0 (placeholder)
            return ast::Stmt::make_expr(ast::Expr::make_int(0));
        }
        ast::Expr e = parse_expr();
        expect(TokKind::Semicolon);
        return ast::Stmt::make_expr(std::move(e));
    }

    // ---- Expression parsing ----

    ast::Expr parse_expr() {
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
                consume(); // '('
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

    // ---- Two-token lookahead for is_colons_decl ----
    // Override the broken stub above with a proper implementation.
    // We need to see: Ident followed immediately by ColonColon.
    // Since peek() doesn't consume, we can do a two-token check by peeking,
    // then calling next() once (on a temporary Lexer copy) — but that's complex.
    // Simpler: since is_colons_decl() is only called when peek() == Ident,
    // we know the first token is Ident.  We need to check token[1] == ColonColon.
    // Do this by consuming the Ident (into a temp), peeking, then "un-consuming"
    // via a flag. But our Lexer has no undo.
    //
    // Alternative approach: save lexer state by using a wrapper.
    // Actually, since peek() is side-effect-free (it saves/restores pos internally),
    // and we only call is_colons_decl() ONCE per potential declaration, we can just:
    // - Use a look-ahead queue of two tokens instead.
    // For now: peek token[0] = Ident, and we need token[1]. We can peek() twice
    // using a temp copy of the lexer.

    Lexer lex_;

    // Re-implement is_colons_decl using a second Lexer copy approach.
    // (Replaces the stub above.)
};

// ----------------------------------------------------------------------------
// We need is_colons_decl to be viable.  The cleanest way without refactoring
// the whole Lexer is to call peek() (which restores position) then call next()
// to get the first token, then peek() again for the second — but our Parser
// already has consume() / peek().
//
// The issue: is_colons_decl() calls consume() and then check().  But after
// the function returns, the Ident has already been consumed.  We can't unwind.
//
// Fix: change is_colons_decl to NOT consume — instead use a helper that
// calls peek() twice by temporarily advancing and restoring the Lexer state.
// The Lexer exposes peek() which is non-destructive.  We need TWO tokens.
//
// Simplest fix: replace is_colons_decl / parse() with a design that peeks
// further ahead.  But that requires Lexer multi-token lookahead.
//
// PRACTICAL SOLUTION: In parse(), when we see an Ident, we consume it, then
// check what follows.  If '::' → colons_decl; otherwise try other options.
// This requires a "put-back" queue.
// ----------------------------------------------------------------------------

// Let's just rewrite parse() to avoid the lookahead problem by using a
// different control-flow: consume the leading Ident first, then dispatch.

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
            // Skip modifier prefixes before declarations
            while (check_word("inline") || check_word("private") || check_word("extern"))
                consume();

            // @decorator → const function decl
            if (check(TokKind::At)) {
                tu.functions.push_back(parse_at_const_decl());
                continue;
            }

            // 'const' function decl
            if (check(TokKind::KwConst)) {
                tu.functions.push_back(parse_const_decl());
                continue;
            }

            // IDENT '::' (...)  → colons-style function decl
            // IDENT 'operator'  → skip
            if (check(TokKind::Ident)) {
                Token name_tok = consume(); // take the ident

                if (check_word("operator")) {
                    // operator declarations not yet supported — skip until '}'
                    skip_stmt();
                    continue;
                }
                if (check(TokKind::ColonColon)) {
                    // Parse the rest of the colons decl, name is name_tok.text
                    tu.functions.push_back(finish_colons_decl(name_tok.text));
                    continue;
                }
                // Unknown leading ident — just skip to next semicolon/brace
                skip_stmt();
                continue;
            }

            // Unrecognised token — skip
            consume();
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

        // Infer execution model from name prefix
        if (decl.name.rfind("vertex", 0) == 0)
            decl.decorator = "vertex";
        else if (decl.name.rfind("fragment", 0) == 0)
            decl.decorator = "fragment";

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

    ast::Block parse_block() {
        ast::Block block;
        expect(TokKind::LBrace);
        while (!check(TokKind::RBrace) && !check(TokKind::Eof))
            block.stmts.push_back(parse_stmt());
        expect(TokKind::RBrace);
        return block;
    }

    ast::Stmt parse_stmt() {
        if (check(TokKind::KwReturn)) {
            consume();
            if (check(TokKind::Semicolon)) { consume(); return ast::Stmt::make_return(std::nullopt); }
            ast::Expr e = parse_expr();
            expect(TokKind::Semicolon);
            return ast::Stmt::make_return(std::move(e));
        }
        // Skip SPIR-V intrinsics (__spirv, __spirv_glsl, __spirv_type, __spirv_glsl)
        if (check(TokKind::Ident) && peek().text.rfind("__", 0) == 0) {
            skip_stmt();
            return ast::Stmt::make_expr(ast::Expr::make_int(0)); // dummy
        }
        ast::Expr e = parse_expr();
        expect(TokKind::Semicolon);
        return ast::Stmt::make_expr(std::move(e));
    }

    ast::Expr parse_expr() {
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
