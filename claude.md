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
- Defined `ExprKind` enum: IntLiteral, FloatLiteral, Identifier, Call
- `Expr` struct: discriminated by `kind`, carries `int_val`, `float_val`, `name`, `args`
- `StmtKind` enum: Return, ExprStmt
- `Stmt` struct: carries optional `expr`; static factory helpers
- `Block`, `Param`, `FunctionDecl` (with `output_name` for named outputs), `TranslationUnit`

### Phase 2 — Hand-Written Parser (`neko/src/neko_parser.hpp` + `neko_parser.cpp`)
- `Lexer`: hand-written tokenizer supporting:
  `Ident`, `IntLit`, `FloatLit`, `BacktickStr`, `@`, `(`, `)`, `{`, `}`,
  `:`, `:=`, `::`, `$`, `,`, `;`, `const`, `return`, `import`, `expose`
- `NekoDSLParser` (in anonymous namespace): recursive-descent, handles:
  - `@decorator const name := (params) $ [output_name:] return_type? { body }`
  - `const name := (params) $ [output_name:] return_type? { body }`
  - `name :: (params) $ [output_name:] return_type? { body }` (decorator inferred from name prefix)
  - `import`/`expose` directives skipped
  - Float literals, integer literals, call expressions, identifier references
  - `vec4(...)` etc. parsed as Call expressions
- `parse_source(const std::string&) -> ast::TranslationUnit` (free function)

### Phase 3 — Code Generator (`neko/src/code_generator.cpp`)
- Added `generate(const ast::TranslationUnit& ast, Options options)`:
  - **Pass 1**: allocate all SPIR-V IDs — types (void, scalars, vecs, pointer types),
    func-types, func IDs, int/float constant IDs, non-entry-point param IDs,
    interface variable IDs (Input/Output per entry-point parameter/output)
  - **Pass 2**: emit function bodies — entry-points use `OpLoad` for each input
    interface var, `OpStore` to output var before `OpReturn`, no SPIR-V params;
    regular functions use `OpFunctionParameter` + `OpReturnValue`;
    `vec2/vec3/vec4` call expressions emit `OpCompositeConstruct`
  - **Pass 3**: final module in layout order: header → capabilities → memory model
    → entry-points (with interface var IDs) → execution modes → debug → annotations
    (Location + BuiltIn OpDecorate) → types (void, float, int, bool, vec2/3/4,
    pointer types, function types) → constants (int + float) → global OpVariables
    → function bodies
- Topological sort ensures callees precede callers in the module
- Supported types: `void`, `int` (i32), `float` (f32), `bool`, `vec2/3/4`
- Supported execution models: `vertex` → Vertex, `fragment` → Fragment
- Built-in decorations: `neko_position` → BuiltIn Position; others → Location 0
- Input params → Location 0, 1, 2 … (ascending per param)
- **Key invariant**: `spv_func_type_key` for entry-points returns `"void"` (same
  as a plain void→void function) so they share a single `OpTypeFunction %void`

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
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` = 480 bytes, SPIR-V validation passes |

## What Is Left To Do

1. **OpVariable / Locals** — local variable declarations (`let x : T = ...`).
2. **Arithmetic Operators** — OpFAdd, OpFMul, OpFNegate, OpIAdd, etc.
3. **STL Module Import** — `import "stl.neko"` / `expose namespace` are skipped;
   need to actually load and compile imported modules.
4. **Richer Built-in Decorations** — multiple vertex inputs, multi-location outputs,
   interpolation modes, uniform buffers, texture bindings.
5. **SPIR-V Optimisation** — hook up `SPIRV-Tools-opt` passes per `optimizationLevel`.
6. **Extended Test Suite** — tests for vec types, float literals, interface variables,
   decorations, multi-function pipelines.
