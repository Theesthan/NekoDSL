# NekoDSL Compiler — Project Memory

## What Has Been Done

### Project Analysis
- **Core Problem**: `compiler.cpp` had `// parse(source);` commented out; the
  bison grammar in `parser.yy` only matched bare identifiers (no-op); and
  `CodeGenerator::generate()` only emitted the SPIR-V header + capabilities,
  producing an empty 48-byte file for every input.
- **Build**: CMake + MinGW on Windows. Dependencies (SPIRV-Tools, SPIRV-Cross,
  googletest, argparse) are resolved. Bison/re2c are optional via `NEKO_GRAMMAR`
  cmake flag; `parser.cpp` is pre-generated and committed.

### Phase 1 — AST (`neko/src/ast.hpp`)
- Defined `ExprKind` enum: IntLiteral, Identifier, Call
- `Expr` struct: discriminated by `kind`, carries `int_val`, `name`, `args`
- `StmtKind` enum: Return, ExprStmt
- `Stmt` struct: carries optional `expr`; static factory helpers
- `Block`, `Param`, `FunctionDecl`, `TranslationUnit`

### Phase 2 — Hand-Written Parser (`neko/src/neko_parser.hpp` + `neko_parser.cpp`)
- `Lexer`: hand-written character-level tokenizer supporting:
  `Ident`, `IntLit`, `@`, `(`, `)`, `{`, `}`, `:`, `:=`, `$`, `,`, `;`,
  `const`, `return`
- `Parser`: recursive-descent, handles:
  - `@decorator` before function decl
  - `const name := (params) $ return_type? { body } ;?`
  - `return expr?;` and `expr;` statements
  - Call expressions, integer literals, identifier references
- `parse_source(const std::string&) -> ast::TranslationUnit` (free function)

### Phase 3 — Code Generator (`neko/src/code_generator.cpp`)
- Added `generate(const ast::TranslationUnit& ast, Options options)`:
  - **Pass 1**: allocate all SPIR-V IDs (types, func-types, funcs, constants, params)
  - **Pass 2**: emit function bodies into a scratch `Binary`; labels/temp IDs
    allocated here; `emit_expr` lambda handles Int/Ident/Call expressions
  - **Pass 3**: write final binary — header (with correct Bound), capabilities,
    memory model, entry-point declarations, debug, types, constants, functions
- Topological sort ensures called functions appear before callers in the module
- Supported primitive types: `void`, `int` (OpTypeInt 32 1), `float` (OpTypeFloat 32), `bool`
- Supported execution models: `vertex` → Vertex, `fragment` → Fragment
- Entry-point functions use void SPIR-V return type (NekoDSL `$ T` for entry
  points maps to an output interface, not a function return value)

### Phase 4 — Compiler Wiring (`neko/src/compiler.cpp`)
- Called `parse_source(source)` to build the AST
- Passed AST to `cg.generate(ast, options)`

### Phase 5 — Build System (`neko/CMakeLists.txt`)
- Added `src/neko_parser.cpp` to the `neko` static library sources

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `neko_test.exe` — `Compile.Simple` | ✅ PASSED (validate + disassemble) |
| `nekoc.exe -k sample -m nekostl/modules` | ✅ exit 0, `sample.spv` = 352 bytes |

## What Is Left To Do

1. **Extended Grammar** — currently only the `const name :=` syntax is parsed.
   The `name ::` syntax seen in `sample.neko` is not yet supported.
2. **STL Module Import** — `import "stl.neko"` and `expose namespace` are not parsed.
3. **Richer Type System** — `vec2/vec3/vec4/mat4`, user-defined struct types.
4. **OpVariable / Locals** — local variable declarations (`let x : T = ...`).
5. **Arithmetic Operators** — OpFAdd, OpFMul, OpFNegate, etc.
6. **Built-in Decorations** — `neko_position` → gl_Position (BuiltIn Position),
   `neko_color` → output colour attachment (Location 0).
7. **SPIR-V Optimisation** — hook up `SPIRV-Tools-opt` passes per `optimizationLevel`.
