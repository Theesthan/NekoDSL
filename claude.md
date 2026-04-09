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
- `ExprKind`: IntLiteral, FloatLiteral, Identifier, Call, BinaryOp, UnaryOp
- `Expr`: discriminated by `kind`; `name` holds callee / operator; `args` holds
  call args, [lhs,rhs] for BinaryOp, or [operand] for UnaryOp
- `StmtKind`: Return, ExprStmt, VarDecl
- `Stmt`: carries optional `expr`, `var_name`, `var_type`; static factory helpers
- `Block`, `Param`, `FunctionDecl` (with `output_name`), `TranslationUnit`

### Phase 2 — Hand-Written Parser (`neko/src/neko_parser.hpp` + `neko_parser.cpp`)
- **Lexer tokens**: `Ident`, `IntLit`, `FloatLit`, `BacktickStr`, `@`, `(`, `)`,
  `{`, `}`, `:`, `:=`, `::`, `$`, `,`, `;`, `=`, `+`, `-`, `*`, `/`,
  `const`, `return`, `let`, `import`, `expose`
- **`NekoDSLParser`**: recursive-descent
  - `@decorator const name := (params) $ [output_name:] return_type? { body }`
  - `const name := (params) $ [output_name:] return_type? { body }`
  - `name :: (params) $ [output_name:] return_type? { body }` (decorator from prefix)
  - `let name : type = expr ;` and `let name = expr ;` (type-inferred)
  - Operator precedence: additive → multiplicative → unary → primary
  - Parenthesised expressions, float/int literals, identifier references, calls
  - `import`/`expose` directives skipped
- `parse_source(const std::string&) -> ast::TranslationUnit` (free function)

### Phase 3 — Code Generator (`neko/src/code_generator.cpp`)
- Three-pass generation: ID allocation → function body emission → module assembly
- **Pass 1**: types (void, float, int, bool, vec2/3/4, pointer types), func-types,
  func IDs, int/float constant IDs, non-entry-point param IDs, interface var IDs
- **Pass 2**: `emit_expr` returns `(result_id, type_name)` enabling type-aware opcode
  selection; handles all ExprKinds including BinaryOp and UnaryOp; VarDecl stored
  as SSA locals (name → id + type)
  - Arithmetic: `OpFAdd/FSub/FMul/FDiv` for float/vec, `OpIAdd/ISub/IMul/SDiv` for int
  - Negation: `OpFNegate` / `OpSNegate`
  - `vec2/3/4(…)` calls → `OpCompositeConstruct`
  - Entry-point input params → `OpLoad` from interface vars; output → `OpStore`
- **Pass 3**: header → capabilities → memory model → entry-points (with interface
  var IDs) → execution modes → debug → annotations (Location, BuiltIn) → types
  (void, scalars, vecs, ptr types, func types) → constants → global OpVariables
  → function bodies
- Topological sort ensures callees precede callers
- **Key invariant**: entry-point `spv_func_type_key` returns `"void"` to share a
  single `OpTypeFunction %void` with plain void functions

### Phase 4 — Compiler Wiring (`neko/src/compiler.cpp`)
- `parse_source(source)` → AST → `cg.generate(ast, options)` → SPIR-V binary

### Phase 5 — Build System (`neko/CMakeLists.txt`)
- Added `src/neko_parser.cpp` to the `neko` static library

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `neko_test.exe` — `Compile.Simple` | ✅ PASSED (validate + disassemble) |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` = 624 bytes, SPIR-V validation passes |
| sample.spv disassembly | ✅ OpFMul, OpFSub, OpFunctionCall, OpCompositeConstruct present |

## What Is Left To Do

1. **Mutable Locals / OpVariable Function** — `let mut x : T = …` with
   OpVariable + OpStore/OpLoad for variables that are reassigned.
2. **Arithmetic with mixed types** — e.g. int literal in a float expression needs
   `OpConvertSToF`; currently assumes both operands have the same type.
3. **STL Module Import** — `import "stl.neko"` skipped; need to load + compile.
4. **Richer Built-in Decorations** — multiple vertex inputs, uniform buffers,
   texture samplers, interpolation modes.
5. **SPIR-V Optimisation** — hook up `SPIRV-Tools-opt` passes per `optimizationLevel`.
6. **Extended Test Suite** — tests for arithmetic, let bindings, vec types,
   interface variables, multi-function shaders.
