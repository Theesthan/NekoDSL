#include "neko_parser.hpp"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace neko {

// ============================================================================
// Lexer
// ============================================================================

enum class TokKind {
    Ident,     // [a-zA-Z_][a-zA-Z_0-9]*
    IntLit,    // [0-9]+
    At,        // @
    LParen,    // (
    RParen,    // )
    LBrace,    // {
    RBrace,    // }
    Colon,     // :
    ColonEq,   // :=
    Dollar,    // $
    Comma,     // ,
    Semicolon, // ;
    KwConst,   // const
    KwReturn,  // return
    Eof
};

struct Token {
    TokKind     kind     = TokKind::Eof;
    std::string text;    // populated for Ident
    int64_t     int_val  = 0; // populated for IntLit
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src), pos_(0) {}

    Token next() {
        skip_ws_and_comments();
        if (pos_ >= src_.size()) return {};

        const char c = src_[pos_];

        // Identifier / keyword
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string text;
            while (pos_ < src_.size() &&
                   (std::isalnum(static_cast<unsigned char>(src_[pos_])) ||
                    src_[pos_] == '_')) {
                text += src_[pos_++];
            }
            if (text == "const")  return { TokKind::KwConst,  text };
            if (text == "return") return { TokKind::KwReturn, text };
            return { TokKind::Ident, text };
        }

        // Integer literal
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string text;
            while (pos_ < src_.size() &&
                   std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                text += src_[pos_++];
            }
            return { TokKind::IntLit, text, std::stoll(text) };
        }

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
            if (pos_ < src_.size() && src_[pos_] == '=') { ++pos_; return { TokKind::ColonEq }; }
            return { TokKind::Colon };
        default:
            // Unknown character: skip and try again
            return next();
        }
    }

    // Non-consuming look-ahead
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
                // Line comment
                while (pos_ < src_.size() && src_[pos_] != '\n') ++pos_;
            } else if (pos_ + 1 < src_.size() && c == '/' && src_[pos_ + 1] == '*') {
                // Block comment
                pos_ += 2;
                while (pos_ + 1 < src_.size() &&
                       !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    ++pos_;
                }
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
            tu.functions.push_back(parse_function_decl());
        }
        return tu;
    }

private:
    // ---- Token helpers ----

    Token consume()                { return lex_.next(); }
    Token peek()                   { return lex_.peek(); }
    bool  check(TokKind k)         { return peek().kind == k; }

    bool match(TokKind k) {
        if (check(k)) { consume(); return true; }
        return false;
    }

    Token expect(TokKind k) {
        Token t = consume();
        if (t.kind != k) {
            throw std::runtime_error(
                std::string("NekoDSL parse error: unexpected token '") +
                t.text + "'");
        }
        return t;
    }

    // ---- Grammar rules ----

    // function_decl → decorator? 'const' IDENT ':=' '(' param_list ')' '$' type? block ';'?
    ast::FunctionDecl parse_function_decl() {
        ast::FunctionDecl decl;

        // Optional decorator: @IDENT
        if (check(TokKind::At)) {
            consume();
            decl.decorator = expect(TokKind::Ident).text;
        }

        expect(TokKind::KwConst);
        decl.name = expect(TokKind::Ident).text;
        expect(TokKind::ColonEq);
        expect(TokKind::LParen);

        // param_list → (param (',' param)*)?
        if (!check(TokKind::RParen)) {
            do {
                ast::Param p;
                p.name = expect(TokKind::Ident).text;
                expect(TokKind::Colon);
                p.type = expect(TokKind::Ident).text;
                decl.params.push_back(std::move(p));
            } while (match(TokKind::Comma));
        }
        expect(TokKind::RParen);

        // '$' return_type?
        expect(TokKind::Dollar);
        if (check(TokKind::Ident)) {
            decl.return_type = consume().text;
        }
        // else: void (return_type stays empty)

        decl.body = parse_block();
        match(TokKind::Semicolon); // optional trailing ';'
        return decl;
    }

    // block → '{' stmt* '}'
    ast::Block parse_block() {
        ast::Block block;
        expect(TokKind::LBrace);
        while (!check(TokKind::RBrace) && !check(TokKind::Eof)) {
            block.stmts.push_back(parse_stmt());
        }
        expect(TokKind::RBrace);
        return block;
    }

    // stmt → 'return' expr? ';'
    //      | expr ';'
    ast::Stmt parse_stmt() {
        if (check(TokKind::KwReturn)) {
            consume();
            if (check(TokKind::Semicolon)) {
                consume();
                return ast::Stmt::make_return(std::nullopt);
            }
            ast::Expr e = parse_expr();
            expect(TokKind::Semicolon);
            return ast::Stmt::make_return(std::move(e));
        }
        ast::Expr e = parse_expr();
        expect(TokKind::Semicolon);
        return ast::Stmt::make_expr(std::move(e));
    }

    // expr → INT_LIT
    //      | IDENT ('(' arg_list? ')')?
    ast::Expr parse_expr() {
        if (check(TokKind::IntLit)) {
            Token t = consume();
            return ast::Expr::make_int(t.int_val);
        }
        if (check(TokKind::Ident)) {
            std::string name = consume().text;
            if (check(TokKind::LParen)) {
                // Function call
                consume();
                std::vector<ast::Expr> args;
                if (!check(TokKind::RParen)) {
                    do {
                        args.push_back(parse_expr());
                    } while (match(TokKind::Comma));
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

// ============================================================================
// Public API
// ============================================================================

ast::TranslationUnit parse_source(const std::string& source) {
    Parser p(source);
    return p.parse();
}

} // namespace neko
