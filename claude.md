# NekoDSL Compiler — Project Memory

## What Has Been Done

### Project Analysis
- **Core Problem**: `compiler.cpp` had `// parse(source);` commented out; bison
  grammar matched bare identifiers only; `CodeGenerator::generate()` emitted only
  the 48-byte SPIR-V header for every input.
- **Build**: CMake + MinGW on Windows. Dependencies (SPIRV-Tools, SPIRV-Cross,
  googletest, argparse) resolved as submodules. Bison/re2c optional via
  `NEKO_GRAMMAR`; `parser.cpp` is pre-generated and committed.

### Phase 1 — AST (`neko/src/ast.hpp`)
- `ExprKind`: IntLiteral, FloatLiteral, Identifier, Call, BinaryOp, UnaryOp
- `Expr.name` — callee / operator string; `Expr.args` — [lhs,rhs] / [operand] / call args
- `StmtKind`: Return, ExprStmt, VarDecl, Assign
- `Stmt.is_mutable` — distinguishes `let` vs `let mut`; `Stmt.var_name` used by both
  VarDecl and Assign
- `Block`, `Param`, `FunctionDecl` (with `output_name`), `TranslationUnit`

### Phase 2 — Hand-Written Parser (`neko/src/neko_parser.hpp` + `neko_parser.cpp`)
- **Tokens**: `Ident`, `IntLit`, `FloatLit`, `BacktickStr`, `@`, `(`, `)`, `{`, `}`,
  `:`, `:=`, `::`, `$`, `,`, `;`, `=`, `+`, `-`, `*`, `/`,
  `const`, `return`, `let`, `mut`, `import`, `expose`
- **`NekoDSLParser`**: recursive-descent
  - `@decorator const name := (params) $ [out:] type? { body }`
  - `const name := (params) $ [out:] type? { body }`
  - `name :: (params) $ [out:] type? { body }` (decorator from name prefix)
  - `let [mut] name [: type] (= | :=) expr ;`
  - `name = expr ;` (assignment to mutable variable)
  - Operator precedence: additive → multiplicative → unary → primary
  - Parenthesised expressions, `import`/`expose` skipped
- `parse_source(const std::string&) -> ast::TranslationUnit`

### Phase 3 — Code Generator (`neko/src/code_generator.cpp`)
- Three-pass: ID allocation → function body emission → module assembly
- **emit_expr** returns `(result_id, type_name)`; threads `locals` (SSA) and
  `mut_locals` (OpVariable-backed) maps
- **Arithmetic**: `OpFAdd/FSub/FMul/FDiv` (float/vec), `OpIAdd/ISub/IMul/SDiv` (int)
- **Negation**: `OpFNegate` / `OpSNegate`
- **Implicit coercion**: int → float via `OpConvertSToF` in BinaryOp and vec constructors
- **Immutable locals** (`let`): SSA value stored directly in `locals` map
- **Mutable locals** (`let mut`):
  - Pass 1 pre-allocates `OpTypePointer Function T` + `OpVariable` IDs
  - Pass 2 emits `OpVariable` immediately after `OpLabel` (SPIR-V requirement)
  - Init value → `OpStore`; reads → `OpLoad` emitted at point of use
  - Reassignment (`name = expr`) → `OpStore`
- **Interface variables**: Input → `OpLoad`; Output → `OpStore` before return; IDs in `OpEntryPoint`
- **Decorations**: `neko_position` → BuiltIn Position; others → Location 0
- **Topological sort**: callees before callers
- **Key invariant**: entry-points and void functions share a single `OpTypeFunction %void`

### Phase 4 — Compiler Wiring (`neko/src/compiler.cpp`)
- `parse_source(source)` → AST → `cg.generate(ast, options)` → SPIR-V binary

### Phase 5 — Build System (`neko/CMakeLists.txt`)
- Added `src/neko_parser.cpp` to the `neko` static library

### Phase 6 — Control Flow (`neko/src/ast.hpp`, `neko_parser.cpp`, `code_generator.cpp`)
- **AST**: `StmtKind::If`; `Stmt.then_block` + `Stmt.else_block` as `unique_ptr<Block>`;
  `Stmt` made non-copyable (movable); `Stmt::make_if` defined out-of-line after `Block`
- **Parser tokens**: `EqEq`, `BangEq`, `Lt`, `LtEq`, `Gt`, `GtEq`, `KwIf`, `KwElse`
- **Parser**: comparison precedence layer (`parse_comparison` → `parse_additive`);
  `if (cond) { ... } [else { ... }]` in `parse_stmt`
- **Codegen**: structured if-else with `OpSelectionMerge` + `OpBranchConditional`;
  Y-combinator auto-lambda (`emit_stmts_impl`) for safe recursion into nested blocks;
  `bool` type registered when any `if` is present; `OpUnreachable` when both branches terminate
- **Comparison opcodes**: `OpFOrd{Equal,NotEqual,LessThan,...}` for float;
  `OpI{Equal,NotEqual}` / `OpS{LessThan,...}` for int
- **Bug fixed**: literal scanners (`all_float_literal_bits`, `all_int_literals`) and
  type registration only walked top-level `func.body.stmts`, missing literals/types
  inside nested `then_block`/`else_block`. Fixed by adding recursive `_block` helpers
  (`collect_float_literals_block`, `collect_int_literals_block`, `collect_calls_block`)
  that traverse the full statement tree. Same fix applied to `collect_calls` (topo sort)
  and VarDecl type registration.

### Phase 7 — While Loops (`ast.hpp`, `neko_parser.cpp`, `code_generator.cpp`)
- **AST**: `StmtKind::While`; `make_while(cond, body)` reuses `then_block` for body
- **Parser token**: `KwWhile`; `while (cond) { body }` parsed in `parse_stmt`
- **Codegen structured loop** (`OpLoopMerge`):
  - Current block → `OpBranch %header`
  - `%header`: emit condition, `OpLoopMerge %merge %continue None`, `OpBranchConditional %cond %body %merge`
  - `%body`: body stmts, `OpBranch %continue` (if not terminated)
  - `%continue`: `OpBranch %header` (back-edge)
  - `%merge`: post-loop execution
- `has_branching` (was `has_if`) now also detects `While` for `bool` type registration
- All recursive block scanners (literals, types, calls) already handle `then_block` for While

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `neko_test.exe` — all 31 tests | ✅ 31/31 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (31 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`

### Phase 8 — SPIR-V Optimisation (`neko/src/compiler.cpp`)
- `SPIRV-Tools-opt` was already linked; `optimizer.hpp` included in `compiler.cpp`
- `optimizationLevel == 1` → `RegisterSizePasses()`
- `optimizationLevel >= 2` → `RegisterPerformancePasses()`
- Optimizer runs on the raw binary before validation/disassembly
- Validation still runs after optimization so invalid optimized output is caught
- Tests `OptimizeLevel1` and `OptimizeLevel2` added (33 total)

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `neko_test.exe` — all 33 tests | ✅ 33/33 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (33 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`,
`OptimizeLevel1`, `OptimizeLevel2`

### Phase 9 — Built-in Decorations (`neko/src/code_generator.cpp`)
- **Built-in inputs**: parameters named `neko_frag_coord`, `neko_vertex_index`,
  `neko_instance_index`, `neko_front_facing` get `BuiltIn` decoration instead of `Location`.
  Location counter only increments for non-builtin inputs.
- **Built-in outputs**: `neko_position` → `BuiltIn Position`, `neko_frag_depth` → `BuiltIn FragDepth`.
- **Color output locations**: `neko_color`/`neko_color0` → Location 0;
  `neko_color1` → Location 1; `neko_colorN` → Location N.
- Static lookup tables `kInputBuiltins` / `kOutputBuiltins` keep the logic data-driven.
- Tests: `FragCoordBuiltin`, `VertexIndexBuiltin`, `MixedInputBuiltin`,
  `ColorLocation0`, `ColorLocation1`, `FragDepthBuiltin` (39 total)

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `neko_test.exe` — all 39 tests | ✅ 39/39 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (39 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`,
`OptimizeLevel1`, `OptimizeLevel2`,
`FragCoordBuiltin`, `VertexIndexBuiltin`, `MixedInputBuiltin`,
`ColorLocation0`, `ColorLocation1`, `FragDepthBuiltin`

### Phase 10 — STL Module Import (`ast.hpp`, `neko_parser.cpp`, `compiler.cpp`, `neko.hpp`)
- **AST**: `TranslationUnit.imports` — `std::vector<std::string>` of import paths
- **Parser**: `DQuoteStr` token for `"path"` literals; `parse_import_paths()` collects paths
  from `import("a", "b")` and `expose import("a")` at module level
- **Compiler**: `resolve_module()` searches CWD then `Options.moduleDirs` for `.neko` files;
  `resolve_imports()` recursively merges imported TUs before the caller (depth-first);
  duplicate imports guarded by `visited` set; missing modules silently skipped
- **Options**: `moduleDirs` field (`std::vector<std::string>`) — passed through from CLI
- **CLI**: `nekoc.exe -m <dir>` now forwarded into `Options.moduleDirs`
- No `std::filesystem` — uses portable `std::ifstream` to avoid MinGW DLL issues
- Tests: `ImportModule`, `ExposeImportModule`, `TransitiveImport` (42 total)

### Phase 11 — Uniform Buffer Objects (`ast.hpp`, `neko_parser.cpp`, `code_generator.cpp`, `compiler.cpp`)
- **AST**: `UniformField { name, type }`, `UniformBlock { name, set, binding, fields }`,
  `TranslationUnit.uniforms: std::vector<UniformBlock>`
- **Parser**: `@uniform(set, binding) const Name := { field : type, ... }` parsed in
  `parse_uniform_block()`; dispatcher in `@` handler checks `deco == "uniform"`
- **Codegen Pass 1**: `ubo_std140_alignment()` / `ubo_std140_size()` helpers; `UboInfo` struct
  (struct_type_id, ptr_type_id, var_id, members map); member indices added to `int_const_ids`;
  member pointer types collected in `ubo_member_ptr_ids`
- **Codegen Pass 2** (`emit_expr` Identifier): iterates `ubo_infos` to find field by name →
  emits `OpAccessChain` (member pointer type) + `OpLoad`
- **Codegen Pass 3**: `OpDecorate Block`, `OpMemberDecorate Offset`, `OpDecorate DescriptorSet/Binding`,
  `OpTypeStruct`, `OpTypePointer Uniform` (struct + member), `OpVariable Uniform`;
  UBO var IDs added to all `OpEntryPoint` interface lists (SPIR-V 1.6 requirement)
- **Bug fixed**: `merge_tu()` in `compiler.cpp` was only merging functions — added uniform
  merging so UBOs declared in the main source survive the `resolve_imports` round-trip
- Tests: `UboSingleField`, `UboSingleFieldAccess`, `UboMultiField`, `UboDescriptorSetBinding` (46 total)

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `ctest --test-dir build` | ✅ 46/46 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (46 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`,
`OptimizeLevel1`, `OptimizeLevel2`,
`FragCoordBuiltin`, `VertexIndexBuiltin`, `MixedInputBuiltin`,
`ColorLocation0`, `ColorLocation1`, `FragDepthBuiltin`,
`ImportModule`, `ExposeImportModule`, `TransitiveImport`,
`UboSingleField`, `UboSingleFieldAccess`, `UboMultiField`, `UboDescriptorSetBinding`

### Phase 12 — Texture Samplers (`ast.hpp`, `neko_parser.cpp`, `code_generator.cpp`, `compiler.cpp`)
- **AST**: `SamplerDecl { name, set, binding, type }` added to `TranslationUnit.samplers`
- **Parser**: `@sampler(set, binding) const name : sampler2D;` parsed in `parse_sampler_decl()`
- **Codegen Pass 1**: shared `image_type_id` (OpTypeImage %float 2D 0 0 0 1 Unknown) and
  `sampled_image_type_id` (OpTypeSampledImage); per-sampler `ptr_type_id` (OpTypePointer
  UniformConstant) and `var_id` (OpVariable UniformConstant); `float` + `vec4` auto-registered
- **Codegen Pass 2**: `sample(name, uv)` built-in in emit_expr Call case → `OpLoad` the
  combined sampler variable, then `OpImageSampleImplicitLod %vec4 %result %sampled %uv`
- **Codegen Pass 3**: `OpTypeImage`, `OpTypeSampledImage`, per-sampler `OpTypePointer` +
  `OpVariable UniformConstant`; `OpDecorate DescriptorSet/Binding` per sampler;
  sampler var IDs added to `OpEntryPoint` interface list
- **`merge_tu`** in `compiler.cpp` extended to merge `samplers` list
- Tests: `SamplerBasic`, `SamplerMultiple`, `SamplerWithUbo` (49 total)

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `ctest --test-dir build` | ✅ 49/49 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (49 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`,
`OptimizeLevel1`, `OptimizeLevel2`,
`FragCoordBuiltin`, `VertexIndexBuiltin`, `MixedInputBuiltin`,
`ColorLocation0`, `ColorLocation1`, `FragDepthBuiltin`,
`ImportModule`, `ExposeImportModule`, `TransitiveImport`,
`UboSingleField`, `UboSingleFieldAccess`, `UboMultiField`, `UboDescriptorSetBinding`,
`SamplerBasic`, `SamplerMultiple`, `SamplerWithUbo`

### Phase 13 — Interpolation Modes (`ast.hpp`, `neko_parser.cpp`, `code_generator.cpp`)
- **AST**: `Param.interp` field (`""`, `"flat"`, `"noperspective"`, `"centroid"`)
- **Parser**: optional `@qualifier` after param type in `parse_params()`
- **Codegen**: `OpDecorate Flat/NoPerspective/Centroid` emitted after Location decoration
  for each input interface variable that carries a non-empty `interp` qualifier
- Tests: `InterpFlat`, `InterpNoPerspective`, `InterpCentroid` (52 total)

## Verification Results

| Check | Result |
|-------|--------|
| `cmake --build build -j4` | ✅ Clean build, all targets |
| `ctest --test-dir build` | ✅ 52/52 PASSED |
| `nekoc.exe -k sample` | ✅ exit 0, `sample.spv` validates |

### Test Coverage (52 tests)
`Simple`, `FloatArithmetic`, `IntArithmetic`, `UnaryNegate`, `MixedTypeCoercion`,
`VecTypes`, `VecWithIntLiterals`, `LetBindings`, `VertexInterfaceVar`,
`VertexFragmentPipeline`, `ChainedCalls`, `ColonSyntax`, `ParenExpressions`,
`OperatorPrecedence`, `VoidHelper`, `BareReturn`,
`MutableLocal`, `MutableLocalInLoop`, `MutableLocalMixedWithImmutable`, `MutableLocalIntType`,
`IfNoElse`, `IfElse`, `ComparisonOps`, `IntComparison`, `EarlyReturn`, `NestedIf`,
`WhileBasic`, `WhileIntCounter`, `WhileWithCall`, `WhileInsideIf`, `IfInsideWhile`,
`OptimizeLevel1`, `OptimizeLevel2`,
`FragCoordBuiltin`, `VertexIndexBuiltin`, `MixedInputBuiltin`,
`ColorLocation0`, `ColorLocation1`, `FragDepthBuiltin`,
`ImportModule`, `ExposeImportModule`, `TransitiveImport`,
`UboSingleField`, `UboSingleFieldAccess`, `UboMultiField`, `UboDescriptorSetBinding`,
`SamplerBasic`, `SamplerMultiple`, `SamplerWithUbo`,
`InterpFlat`, `InterpNoPerspective`, `InterpCentroid`

## What Is Left To Do

All originally planned features are implemented. No further work required.
