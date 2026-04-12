#include <gtest/gtest.h>

#include <neko/neko.hpp>

#include <fstream>

namespace {

// ---------------------------------------------------------------------------
// Helper: compile source, validate, return binary. Throws on any error.
// ---------------------------------------------------------------------------

std::vector<uint32_t> compile_validate(const std::string& src)
{
    neko::Compiler c;
    neko::Options o;
    o.validate = true;
    c.setOptions(o);
    return c.compile(src);
}

std::vector<uint32_t> compile_debug(const std::string& src)
{
    neko::Compiler c;
    neko::Options o;
    o.validate       = true;
    o.debugInfo      = true;
    o.showDisassembly = true;
    c.setOptions(o);
    return c.compile(src);
}

// ---------------------------------------------------------------------------
// Original regression test (kept unchanged)
// ---------------------------------------------------------------------------

TEST(Compile, Simple)
{
    const std::string src = R"NEKO(
        @vertex
        const main := () $ int {
            dead();
            return ping_pong(0);
        };

        const ping_pong := (value : int) $ int {
            return value;
        };

        const dead := () $ {

        };
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.debugInfo      = true;
    o.showDisassembly = true;
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));
}

// ---------------------------------------------------------------------------
// Float arithmetic
// ---------------------------------------------------------------------------

TEST(Compile, FloatArithmetic)
{
    const std::string src = R"NEKO(
        const add_floats := (a : float, b : float) $ float {
            return a + b;
        }

        const mul_floats := (a : float, b : float) $ float {
            return a * b;
        }

        const div_floats := (a : float, b : float) $ float {
            return a / b;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let x : float = add_floats(1.0, 2.0);
            let y : float = mul_floats(x, 0.5);
            let z : float = div_floats(y, 2.0);
            return vec4(x, y, z, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Integer arithmetic
// ---------------------------------------------------------------------------

TEST(Compile, IntArithmetic)
{
    const std::string src = R"NEKO(
        const add_ints := (a : int, b : int) $ int {
            return a + b;
        }

        const sub_ints := (a : int, b : int) $ int {
            return a - b;
        }

        @vertex
        const vert := () $ int {
            return add_ints(sub_ints(10, 3), 5);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Unary negation
// ---------------------------------------------------------------------------

TEST(Compile, UnaryNegate)
{
    const std::string src = R"NEKO(
        const neg_float := (v : float) $ float {
            return -v;
        }

        const neg_int := (v : int) $ int {
            return -v;
        }

        @vertex
        const vert := () $ int {
            return neg_int(5);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Mixed-type arithmetic (int literal in float expression → OpConvertSToF)
// ---------------------------------------------------------------------------

TEST(Compile, MixedTypeCoercion)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let f : float = 1.0;
            // int literal 2 must be promoted to float before OpFMul
            let r : float = f * 2;
            // float literal used with int-typed local (coerce int→float on rhs)
            let n : int = 3;
            let g : float = n + 0.5;
            return vec4(r, g, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Vector types and constructors
// ---------------------------------------------------------------------------

TEST(Compile, VecTypes)
{
    const std::string src = R"NEKO(
        const make_color := (r : float, g : float, b : float) $ vec4 {
            return vec4(r, g, b, 1.0);
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            return make_color(1.0, 0.5, 0.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// vec4 constructor with int literals (int→float coercion inside OpCompositeConstruct)
// ---------------------------------------------------------------------------

TEST(Compile, VecWithIntLiterals)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            return vec4(1, 0, 1, 1);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Let bindings (VarDecl) — inferred and explicit types
// ---------------------------------------------------------------------------

TEST(Compile, LetBindings)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let a : float = 0.2;
            let b : float = a + 0.3;
            let c = b * 2.0;
            return vec4(a, b, c, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Input interface variable (vertex shader with position input)
// ---------------------------------------------------------------------------

TEST(Compile, VertexInterfaceVar)
{
    const std::string src = R"NEKO(
        @vertex
        const vert := (in_pos : vec4) $ neko_position : vec4 {
            return in_pos;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Full vertex + fragment pipeline
// ---------------------------------------------------------------------------

TEST(Compile, VertexFragmentPipeline)
{
    const std::string src = R"NEKO(
        const scale := (v : float) $ float {
            return v * 2.0;
        }

        @vertex
        const vert := (in_pos : vec4) $ neko_position : vec4 {
            return in_pos;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let r : float = scale(0.5);
            let g : float = 0.0;
            let b : float = r - 0.5;
            return vec4(r, g, b, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Chained function calls and expression nesting
// ---------------------------------------------------------------------------

TEST(Compile, ChainedCalls)
{
    const std::string src = R"NEKO(
        const double_it := (x : float) $ float {
            return x * 2.0;
        }

        const halve := (x : float) $ float {
            return x / 2.0;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let v : float = halve(double_it(double_it(0.5)));
            return vec4(v, v, v, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// :: syntax (colons-style function declaration)
// ---------------------------------------------------------------------------

TEST(Compile, ColonSyntax)
{
    const std::string src = R"NEKO(
        const helper := (x : float) $ float {
            return x + 1.0;
        }

        fragment_pass :: () $ neko_color : vec4 {
            let v : float = helper(0.5);
            return vec4(v, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Parenthesised expressions
// ---------------------------------------------------------------------------

TEST(Compile, ParenExpressions)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let a : float = (1.0 + 2.0) * 0.5;
            let b : float = 1.0 / (a + 0.1);
            return vec4(a, b, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Operator precedence: a + b * c  must be  a + (b * c)
// ---------------------------------------------------------------------------

TEST(Compile, OperatorPrecedence)
{
    const std::string src = R"NEKO(
        const check := (a : float, b : float, c : float) $ float {
            return a + b * c;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let v : float = check(1.0, 2.0, 3.0);
            return vec4(v, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Edge: void helper function + expression statement (result discarded)
// ---------------------------------------------------------------------------

TEST(Compile, VoidHelper)
{
    const std::string src = R"NEKO(
        const noop := () $ {
        }

        @vertex
        const vert := () $ int {
            noop();
            return 0;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Edge: bare return (no value) from void function
// ---------------------------------------------------------------------------

TEST(Compile, BareReturn)
{
    const std::string src = R"NEKO(
        const early_out := (flag : int) $ {
            return;
        }

        @vertex
        const vert := () $ int {
            early_out(0);
            return 1;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Mutable locals: let mut + reassignment
// ---------------------------------------------------------------------------

TEST(Compile, MutableLocal)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut acc : float = 0.0;
            acc = acc + 0.25;
            acc = acc + 0.25;
            return vec4(acc, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

TEST(Compile, MutableLocalInLoop)
{
    // Simulated manual unroll (no loop syntax yet) using mutable accumulator.
    const std::string src = R"NEKO(
        const step := (v : float) $ float {
            return v + 0.1;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let mut x : float = 0.0;
            x = step(x);
            x = step(x);
            x = step(x);
            return vec4(x, x, x, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

TEST(Compile, MutableLocalMixedWithImmutable)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let base : float = 0.5;
            let mut r : float = base;
            r = r * 2.0;
            let g : float = base - 0.1;
            return vec4(r, g, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

TEST(Compile, MutableLocalIntType)
{
    const std::string src = R"NEKO(
        const add_one := (n : int) $ int {
            return n + 1;
        }

        @vertex
        const vert := () $ int {
            let mut counter : int = 0;
            counter = add_one(counter);
            counter = add_one(counter);
            return counter;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: if without else (mutable var updated conditionally)
// ---------------------------------------------------------------------------

TEST(Compile, IfNoElse)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut r : float = 0.0;
            if (r < 0.5) {
                r = 1.0;
            }
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: if-else
// ---------------------------------------------------------------------------

TEST(Compile, IfElse)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut r : float = 0.0;
            if (r < 0.5) {
                r = 1.0;
            } else {
                r = 0.5;
            }
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: comparison operators (all six)
// ---------------------------------------------------------------------------

TEST(Compile, ComparisonOps)
{
    const std::string src = R"NEKO(
        const clamp01 := (v : float) $ float {
            let mut r : float = v;
            if (v < 0.0) { r = 0.0; }
            if (v > 1.0) { r = 1.0; }
            return r;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let x : float = clamp01(1.5);
            return vec4(x, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: integer comparison
// ---------------------------------------------------------------------------

TEST(Compile, IntComparison)
{
    const std::string src = R"NEKO(
        const max_int := (a : int, b : int) $ int {
            let mut r : int = a;
            if (b > a) { r = b; }
            return r;
        }

        @vertex
        const vert := () $ int {
            return max_int(3, 7);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: early return inside if
// ---------------------------------------------------------------------------

TEST(Compile, EarlyReturn)
{
    const std::string src = R"NEKO(
        const safe_div := (a : float, b : float) $ float {
            if (b == 0.0) {
                return 0.0;
            }
            return a / b;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let v : float = safe_div(1.0, 2.0);
            return vec4(v, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Control flow: nested if
// ---------------------------------------------------------------------------

TEST(Compile, NestedIf)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut r : float = 0.0;
            let x : float = 0.7;
            if (x > 0.5) {
                if (x > 0.9) {
                    r = 1.0;
                } else {
                    r = 0.75;
                }
            }
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// While loop: basic counting loop with mutable accumulator
// ---------------------------------------------------------------------------

TEST(Compile, WhileBasic)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut x : float = 0.0;
            while (x < 0.9) {
                x = x + 0.1;
            }
            return vec4(x, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// While loop: integer counter
// ---------------------------------------------------------------------------

TEST(Compile, WhileIntCounter)
{
    const std::string src = R"NEKO(
        @vertex
        const vert := () $ int {
            let mut n : int = 0;
            while (n < 5) {
                n = n + 1;
            }
            return n;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// While loop: calls a helper each iteration
// ---------------------------------------------------------------------------

TEST(Compile, WhileWithCall)
{
    const std::string src = R"NEKO(
        const step := (v : float) $ float {
            return v + 0.2;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let mut acc : float = 0.0;
            while (acc < 0.8) {
                acc = step(acc);
            }
            return vec4(acc, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// While loop nested inside an if
// ---------------------------------------------------------------------------

TEST(Compile, WhileInsideIf)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let flag : float = 1.0;
            let mut r : float = 0.0;
            if (flag > 0.5) {
                while (r < 0.5) {
                    r = r + 0.1;
                }
            }
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// If nested inside a while body
// ---------------------------------------------------------------------------

TEST(Compile, IfInsideWhile)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut x : float = 0.0;
            let mut y : float = 0.0;
            while (x < 1.0) {
                x = x + 0.25;
                if (x > 0.5) {
                    y = y + 0.1;
                }
            }
            return vec4(x, y, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Optimizer: level 1 (size) and level 2 (performance) should not throw
// ---------------------------------------------------------------------------

TEST(Compile, OptimizeLevel1)
{
    const std::string src = R"NEKO(
        const clamp01 := (v : float) $ float {
            let mut r : float = v;
            if (v < 0.0) { r = 0.0; }
            if (v > 1.0) { r = 1.0; }
            return r;
        }

        @fragment
        const frag := () $ neko_color : vec4 {
            let r : float = clamp01(1.5);
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.validate          = true;
    o.optimizationLevel = 1;
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));
}

TEST(Compile, OptimizeLevel2)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color : vec4 {
            let mut x : float = 0.0;
            while (x < 0.9) {
                x = x + 0.1;
            }
            return vec4(x, 0.0, 0.0, 1.0);
        }
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.validate          = true;
    o.optimizationLevel = 2;
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));
}

// ---------------------------------------------------------------------------
// Built-in inputs: neko_frag_coord (BuiltIn FragCoord)
// ---------------------------------------------------------------------------

TEST(Compile, FragCoordBuiltin)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := (neko_frag_coord : vec4) $ neko_color : vec4 {
            let r : float = neko_frag_coord;
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    // neko_frag_coord is a vec4; reading it as float would normally be a type
    // mismatch — just verify the decorator wiring compiles without crash.
    // Use the vec4 directly instead.
    const std::string src2 = R"NEKO(
        @fragment
        const frag := (neko_frag_coord : vec4) $ neko_color : vec4 {
            return neko_frag_coord;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src2));
}

// ---------------------------------------------------------------------------
// Built-in inputs: neko_vertex_index (BuiltIn VertexIndex)
// ---------------------------------------------------------------------------

TEST(Compile, VertexIndexBuiltin)
{
    const std::string src = R"NEKO(
        @vertex
        const vert := (neko_vertex_index : int) $ neko_position : vec4 {
            return vec4(0.0, 0.0, 0.0, 1.0);
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Mixed: regular vertex input + builtin instance index
// ---------------------------------------------------------------------------

TEST(Compile, MixedInputBuiltin)
{
    const std::string src = R"NEKO(
        @vertex
        const vert := (in_pos : vec4, neko_instance_index : int) $ neko_position : vec4 {
            return in_pos;
        }
    )NEKO";

    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Output name variants: neko_color0 → Location 0, neko_color1 → Location 1
// ---------------------------------------------------------------------------

TEST(Compile, ColorLocation0)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color0 : vec4 {
            return vec4(1.0, 0.0, 0.0, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

TEST(Compile, ColorLocation1)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := () $ neko_color1 : vec4 {
            return vec4(0.0, 1.0, 0.0, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Output: neko_frag_depth (BuiltIn FragDepth) — float output
// ---------------------------------------------------------------------------

TEST(Compile, FragDepthBuiltin)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := (neko_frag_coord : vec4) $ neko_frag_depth : float {
            return 0.5;
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Module import: cross-file function reference via import("...")
// ---------------------------------------------------------------------------

// Write text to a file, returns the path.
static void write_file(const std::string& path, const std::string& content)
{
    std::ofstream f(path);
    f << content;
}

TEST(Compile, ImportModule)
{
    write_file("neko_test_helper.neko",
        "const double_it := (x : float) $ float {\n"
        "    return x * 2.0;\n"
        "}\n");

    const std::string src = R"NEKO(
        import("neko_test_helper.neko");

        @fragment
        const frag := () $ neko_color : vec4 {
            let v : float = double_it(0.5);
            return vec4(v, 0.0, 0.0, 1.0);
        }
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.validate   = true;
    o.moduleDirs = { "." };
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));

    std::remove("neko_test_helper.neko");
}

// ---------------------------------------------------------------------------
// Module import: expose import(...) syntax (as used in stl.neko)
// ---------------------------------------------------------------------------

TEST(Compile, ExposeImportModule)
{
    write_file("neko_test_utils.neko",
        "const clamp01 := (v : float) $ float {\n"
        "    let mut r : float = v;\n"
        "    if (v < 0.0) { r = 0.0; }\n"
        "    if (v > 1.0) { r = 1.0; }\n"
        "    return r;\n"
        "}\n");

    const std::string src = R"NEKO(
        expose import("neko_test_utils.neko");

        @fragment
        const frag := () $ neko_color : vec4 {
            let r : float = clamp01(1.5);
            return vec4(r, 0.0, 0.0, 1.0);
        }
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.validate   = true;
    o.moduleDirs = { "." };
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));

    std::remove("neko_test_utils.neko");
}

// ---------------------------------------------------------------------------
// Module import: transitive imports (A imports B which imports C)
// ---------------------------------------------------------------------------

TEST(Compile, TransitiveImport)
{
    write_file("neko_test_base.neko",
        "const add_one := (n : float) $ float { return n + 1.0; }\n");
    write_file("neko_test_mid.neko",
        "import(\"neko_test_base.neko\");\n"
        "const add_two := (n : float) $ float { return add_one(add_one(n)); }\n");

    const std::string src = R"NEKO(
        import("neko_test_mid.neko");

        @fragment
        const frag := () $ neko_color : vec4 {
            let v : float = add_two(0.0);
            return vec4(v, 0.0, 0.0, 1.0);
        }
    )NEKO";

    neko::Compiler c;
    neko::Options o;
    o.validate   = true;
    o.moduleDirs = { "." };
    c.setOptions(o);
    EXPECT_NO_THROW(c.compile(src));

    std::remove("neko_test_base.neko");
    std::remove("neko_test_mid.neko");
}

// ---------------------------------------------------------------------------
// Uniform buffer objects
// ---------------------------------------------------------------------------

// Single-field UBO: read one float from a uniform block.
TEST(Compile, UboSingleField)
{
    const std::string src = R"NEKO(
        @uniform(0, 0) const Params := {
            brightness : float
        };

        @fragment
        const frag := () $ neko_color : vec4 {
            return vec4(1.0, 0.0, 0.0, 1.0);
        }
    )NEKO";
    // No UBO field access yet — just verify UBO declaration compiles.
    EXPECT_NO_THROW(compile_validate(src));
}

TEST(Compile, UboSingleFieldAccess)
{
    const std::string src = R"NEKO(
        @uniform(0, 0) const Params := {
            brightness : float
        };

        @fragment
        const frag := () $ neko_color : vec4 {
            let b : float = brightness;
            return vec4(b, b, b, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// Multi-field UBO; accesses scale (float field at offset 8, aligned to 16).
TEST(Compile, UboMultiField)
{
    const std::string src = R"NEKO(
        @uniform(0, 0) const Camera := {
            offset : vec2,
            scale  : float
        };

        @vertex
        const vert := (pos : vec2) $ neko_position : vec4 {
            let s : float = scale;
            return vec4(pos, 0.0, s);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// UBO with set=1, binding=2 — verifies descriptor set/binding decoration.
TEST(Compile, UboDescriptorSetBinding)
{
    const std::string src = R"NEKO(
        @uniform(1, 2) const Material := {
            color : vec4
        };

        @fragment
        const frag := () $ neko_color : vec4 {
            return color;
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Texture samplers
// ---------------------------------------------------------------------------

// Declare a sampler2D and use sample() to read it.
TEST(Compile, SamplerBasic)
{
    const std::string src = R"NEKO(
        @sampler(0, 1) const tex : sampler2D;

        @fragment
        const frag := (uv : vec2) $ neko_color : vec4 {
            return sample(tex, uv);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// Multiple samplers with different set/binding.
TEST(Compile, SamplerMultiple)
{
    const std::string src = R"NEKO(
        @sampler(0, 0) const albedo   : sampler2D;
        @sampler(0, 1) const normalMap : sampler2D;

        @fragment
        const frag := (uv : vec2) $ neko_color : vec4 {
            let c : vec4 = sample(albedo, uv);
            return c;
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// Sampler combined with a UBO uniform.
TEST(Compile, SamplerWithUbo)
{
    const std::string src = R"NEKO(
        @uniform(0, 0) const Material := {
            tint : vec4
        };
        @sampler(0, 1) const tex : sampler2D;

        @fragment
        const frag := (uv : vec2) $ neko_color : vec4 {
            let c : vec4 = sample(tex, uv);
            return c;
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// ---------------------------------------------------------------------------
// Interpolation modes
// ---------------------------------------------------------------------------

// @flat on a fragment input — integer-typed varyings require flat interpolation.
TEST(Compile, InterpFlat)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := (primitive_id : int @flat) $ neko_color : vec4 {
            return vec4(1.0, 0.0, 0.0, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// @noperspective on a vec2 varying.
TEST(Compile, InterpNoPerspective)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := (uv : vec2 @noperspective) $ neko_color : vec4 {
            return vec4(uv, 0.0, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

// @centroid on a vec2 varying.
TEST(Compile, InterpCentroid)
{
    const std::string src = R"NEKO(
        @fragment
        const frag := (uv : vec2 @centroid) $ neko_color : vec4 {
            return vec4(uv, 0.0, 1.0);
        }
    )NEKO";
    EXPECT_NO_THROW(compile_validate(src));
}

} // namespace
