# NekoDSL Language Reference

NekoDSL is a GPU shader language that compiles directly to SPIR-V binary — the intermediate format consumed by Vulkan and OpenGL 4.6+. It is designed to be minimal, explicit, and close to the metal, with a syntax that maps cleanly onto SPIR-V concepts.

```
source.neko  ──►  nekoc  ──►  source.spv  ──►  Vulkan / OpenGL driver  ──►  GPU
```

---

## Table of Contents

1. [Getting Started](#getting-started)
2. [CLI Reference — nekoc](#cli-reference--nekoc)
3. [C++ API Reference](#c-api-reference)
4. [Language Overview](#language-overview)
5. [Types](#types)
6. [Variables](#variables)
7. [Expressions & Operators](#expressions--operators)
8. [Control Flow](#control-flow)
9. [Functions](#functions)
10. [Entry Points](#entry-points)
11. [Built-in Interface Variables](#built-in-interface-variables)
12. [Uniform Buffers](#uniform-buffers)
13. [Texture Samplers](#texture-samplers)
14. [Interpolation Modes](#interpolation-modes)
15. [Module Imports](#module-imports)
16. [SPIR-V Optimization](#spir-v-optimization)
17. [Complete Examples](#complete-examples)
18. [Use Cases](#use-cases)
19. [Limitations](#limitations)

---

## Getting Started

### Prerequisites

- CMake ≥ 3.15
- A C++17 compiler (MinGW-w64 / GCC / Clang / MSVC)
- Python ≥ 3 (required by SPIRV-Tools build system)

### Build

```sh
git clone <repo-url>
cd NekoDSL
git submodule update --init --recursive

mkdir build && cd build
cmake ..
cmake --build . -j4
```

The build produces:
- `build/neko/libneko.a` — static library (embed in your engine)
- `build/nekoc/nekoc.exe` — command-line compiler

---

## CLI Reference — nekoc

```
nekoc -k <source> [source2 ...] [options]
```

| Flag | Description |
|------|-------------|
| `-k <file> [file2 ...]` | Source file(s) to compile (`.neko` extension added automatically) |
| `-m <dir> [dir2 ...]` | Module search directories for `import(...)` resolution |
| `-o <level>` | Optimization level: `0` = none, `1` = size passes, `2`+ = performance passes |
| `-v` | Verbose — validate and print SPIR-V disassembly to stdout |
| `-g` | Emit debug info (OpSource, OpName) |

**Exit codes:**

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Argument parse error |
| `2` | Invalid compiler options |
| `3` | Compilation / validation error |

**Examples:**

```sh
# Compile shader.neko → shader.spv
nekoc -k shader

# Compile with validation and disassembly output
nekoc -k shader -v

# Compile with optimization and a module search path
nekoc -k shader -o 2 -m shaders/lib

# Compile multiple files
nekoc -k vert frag -m lib
```

> **Note:** The `.neko` extension must be omitted from `-k` arguments. The compiler appends it automatically.

---

## C++ API Reference

Include `<neko/neko.hpp>` and link against `libneko.a`.

### `neko::Options`

```cpp
struct Options {
    bool     debugInfo        = false;  // emit OpSource / OpName
    bool     validate         = true;   // run SPIR-V validation after codegen
    bool     showDisassembly  = false;  // print human-readable SPIR-V to stdout
    bool     profile          = false;  // reserved
    uint8_t  optimizationLevel = 0;    // 0=none, 1=size, 2+=perf
    std::vector<std::string> moduleDirs; // search paths for import()
};
```

### `neko::Compiler`

```cpp
class Compiler {
public:
    std::vector<uint32_t> compile(const std::string& source) const;
    const Options& getOptions() const;
    void setOptions(const Options& newOptions); // throws on invalid options
};
```

### Minimal usage

```cpp
#include <neko/neko.hpp>
#include <fstream>

int main() {
    std::string src = R"(
        @vertex
        const vert := (pos : vec4) $ neko_position : vec4 {
            return pos;
        }
    )";

    neko::Compiler c;
    neko::Options o;
    o.validate = true;
    c.setOptions(o);

    std::vector<uint32_t> spirv = c.compile(src);

    // Write to file
    std::ofstream f("out.spv", std::ios::binary);
    f.write(reinterpret_cast<const char*>(spirv.data()),
            spirv.size() * sizeof(uint32_t));
}
```

### Error handling

`compile()` throws `std::runtime_error` on parse errors, type errors, and (when `validate=true`) SPIR-V validation failures.

```cpp
try {
    auto spirv = compiler.compile(source);
} catch (const std::runtime_error& e) {
    std::cerr << "Shader error: " << e.what() << '\n';
}
```

---

## Language Overview

NekoDSL files use the `.neko` extension. A file may contain any combination of:

- Helper function declarations (`const name := (params) $ return_type { ... }`)
- Entry point declarations (`@vertex` / `@fragment`)
- Uniform buffer declarations (`@uniform`)
- Sampler declarations (`@sampler`)
- Import statements (`import("file.neko")`)

Comments use `//` (line) or `/* */` (block).

---

## Types

| NekoDSL type | SPIR-V type | Description |
|---|---|---|
| `float` | `OpTypeFloat 32` | 32-bit IEEE float |
| `int` | `OpTypeInt 32 1` | 32-bit signed integer |
| `bool` | `OpTypeBool` | Boolean (comparison results) |
| `vec2` | `OpTypeVector float 2` | 2-component float vector |
| `vec3` | `OpTypeVector float 3` | 3-component float vector |
| `vec4` | `OpTypeVector float 4` | 4-component float vector |
| `sampler2D` | `OpTypeSampledImage` | Combined image+sampler (2D) |

**Vector construction** uses function call syntax:

```neko
let v2 : vec2 = vec2(0.5, 1.0);
let v4 : vec4 = vec4(v2, 0.0, 1.0);  // swizzle-extend: vec2 + float + float
let v3 : vec3 = vec3(1.0, 0.0, 0.0);
```

**Implicit int → float promotion** happens automatically in mixed arithmetic and vector constructors:

```neko
let x : vec4 = vec4(1, 0, 0, 1);  // int literals promoted to float
```

---

## Variables

### Immutable binding (`let`)

```neko
let name : type = expr;
let name := expr;         // inferred type
```

Immutable locals are SSA values — they can never be reassigned.

### Mutable binding (`let mut`)

```neko
let mut name : type = expr;
```

Mutable locals are backed by `OpVariable Function` in SPIR-V and can be reassigned:

```neko
let mut total : float = 0.0;
total = total + 1.0;
```

### Assignment

```neko
name = expr;   // only valid if name was declared with let mut
```

---

## Expressions & Operators

### Arithmetic

| Operator | Description |
|---|---|
| `+` `-` `*` `/` | Add, subtract, multiply, divide |
| `-expr` | Unary negation |

Float operations map to `OpFAdd/FSub/FMul/FDiv`; integer operations map to `OpIAdd/ISub/IMul/SDiv`.

### Comparison

| Operator | Meaning |
|---|---|
| `==` `!=` | Equal / not equal |
| `<` `<=` | Less than / less or equal |
| `>` `>=` | Greater than / greater or equal |

Comparisons return `bool` and are used in `if`/`while` conditions.

### Operator precedence (high → low)

```
unary (-)  →  * /  →  + -  →  == != < <= > >=
```

### Parentheses

```neko
let x : float = (a + b) * c;
```

### Built-in functions

| Function | Signature | Description |
|---|---|---|
| `vec2(...)` | `(float, float)` → `vec2` | Construct vec2 |
| `vec3(...)` | `(float, float, float)` or `(vec2, float)` → `vec3` | Construct vec3 |
| `vec4(...)` | Multiple overloads → `vec4` | Construct vec4 |
| `sample(tex, uv)` | `(sampler2D, vec2)` → `vec4` | Sample texture at UV |

---

## Control Flow

### If / else

```neko
if (condition) {
    // then branch
}

if (condition) {
    // then branch
} else {
    // else branch
}
```

Generates `OpSelectionMerge` + `OpBranchConditional` (structured control flow).

### While loop

```neko
while (condition) {
    // body
}
```

Generates `OpLoopMerge` + back-edge (structured loop).

### Return

```neko
return;           // void return
return expr;      // return a value
```

Entry points (`@vertex` / `@fragment`) store the return value to the output interface variable before `OpReturn`.

---

## Functions

### Declaration syntax

```neko
const name := (param1 : type1, param2 : type2) $ return_type {
    // body
}
```

Use `$` with no identifier for void:

```neko
const init := (x : float) $ {
    // void function
}
```

### Calling functions

```neko
let result : float = my_func(a, b);
my_void_func(x);
```

The compiler topologically sorts functions so callees always appear before callers in SPIR-V — no forward declarations needed.

### Alternative declaration syntax (name-prefix)

Functions whose names start with `vertex` or `fragment` are automatically treated as entry points:

```neko
vertex_main :: (pos : vec3) $ neko_position : vec4 {
    return vec4(pos, 1.0);
}
```

---

## Entry Points

Entry points are declared with `@vertex` or `@fragment`:

```neko
@vertex
const vert := (params) $ output_name : output_type {
    ...
}

@fragment
const frag := (params) $ output_name : output_type {
    ...
}
```

- **Parameters** become input interface variables (Location-decorated).
- **`$ output_name : output_type`** declares the output interface variable.
- Entry points always return `void` in SPIR-V; the `return expr` statement is automatically rewritten to `OpStore` + `OpReturn`.

---

## Built-in Interface Variables

These parameter and output names trigger automatic `BuiltIn` decoration instead of `Location`.

### Built-in inputs

| Name | SPIR-V BuiltIn | Type | Shader stage |
|---|---|---|---|
| `neko_frag_coord` | `FragCoord` | `vec4` | Fragment |
| `neko_vertex_index` | `VertexIndex` | `int` | Vertex |
| `neko_instance_index` | `InstanceIndex` | `int` | Vertex |
| `neko_front_facing` | `FrontFacing` | `bool` | Fragment |

```neko
@fragment
const frag := (neko_frag_coord : vec4) $ neko_color : vec4 {
    return vec4(neko_frag_coord, 0.0, 1.0);
}
```

### Built-in outputs

| Name | SPIR-V BuiltIn | Type | Shader stage |
|---|---|---|---|
| `neko_position` | `Position` | `vec4` | Vertex |
| `neko_frag_depth` | `FragDepth` | `float` | Fragment |

### Color outputs (Location-decorated)

| Name | Location |
|---|---|
| `neko_color` / `neko_color0` | 0 |
| `neko_color1` | 1 |
| `neko_color2` | 2 |
| `neko_colorN` | N |

---

## Uniform Buffers

Uniform Buffer Objects (UBOs) pass CPU data to all shader stages. Layout follows std140 rules.

### Declaration

```neko
@uniform(set, binding) const BlockName := {
    field1 : type,
    field2 : type
};
```

### Accessing fields

Field names are accessible directly as identifiers anywhere inside a shader body:

```neko
@uniform(0, 0) const Transform := {
    model      : vec4,
    view       : vec4,
    projection : vec4
};

@fragment
const frag := () $ neko_color : vec4 {
    let m : vec4 = model;       // reads Transform.model
    let v : vec4 = view;        // reads Transform.view
    return vec4(1.0, 0.0, 0.0, 1.0);
}
```

### std140 offsets (auto-calculated)

| Type | Alignment | Size |
|---|---|---|
| `float` / `int` | 4 bytes | 4 bytes |
| `vec2` | 8 bytes | 8 bytes |
| `vec3` | 16 bytes | 12 bytes |
| `vec4` | 16 bytes | 16 bytes |

### Vulkan binding (C++ side)

```cpp
// The UBO is bound at set=0, binding=0 as declared
VkDescriptorSetLayoutBinding uboBinding{};
uboBinding.binding         = 0;
uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
uboBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
```

---

## Texture Samplers

Declares a combined image sampler (the most common form in Vulkan).

### Declaration

```neko
@sampler(set, binding) const name : sampler2D;
```

### Sampling

```neko
let color : vec4 = sample(name, uv);  // uv must be vec2
```

`sample()` maps to `OpImageSampleImplicitLod` — implicit LOD selection using derivatives (only valid in fragment shaders with divergent control flow).

### Example

```neko
@sampler(0, 1) const albedo    : sampler2D;
@sampler(0, 2) const normalMap : sampler2D;

@fragment
const frag := (uv : vec2) $ neko_color : vec4 {
    let diffuse : vec4 = sample(albedo, uv);
    let normal  : vec4 = sample(normalMap, uv);
    return diffuse;
}
```

### Vulkan binding (C++ side)

```cpp
VkDescriptorSetLayoutBinding samplerBinding{};
samplerBinding.binding        = 1;  // matches @sampler(0, 1)
samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
samplerBinding.stageFlags     = VK_SHADER_STAGE_FRAGMENT_BIT;
```

---

## Interpolation Modes

Fragment shader input parameters can carry an interpolation qualifier after the type:

```neko
param : type @qualifier
```

| Qualifier | SPIR-V Decoration | Meaning |
|---|---|---|
| *(none)* | *(perspective correct, default)* | Standard interpolation |
| `@flat` | `Flat` | No interpolation — last vertex value used. **Required for integer varyings.** |
| `@noperspective` | `NoPerspective` | Linear interpolation without perspective correction |
| `@centroid` | `Centroid` | Centroid sampling (MSAA-safe interpolation) |

```neko
@fragment
const frag := (
    color     : vec4,           // perspective-correct (default)
    prim_id   : int @flat,      // flat — required for int
    uv_linear : vec2 @noperspective,
    uv_safe   : vec2 @centroid
) $ neko_color : vec4 {
    return color;
}
```

---

## Module Imports

Split shader code across multiple files using `import`.

### Syntax

```neko
import("path/to/module.neko");
import("module");               // .neko extension added automatically
```

### `expose import`

```neko
expose import("lib.neko");      // marks as public re-export (resolved same way)
```

### Resolution order

1. Exact path as written (relative to CWD or absolute)
2. Path with `.neko` appended
3. Each `-m <dir>` search directory in order

Missing modules are silently skipped (no error). Circular imports are safe — each module is only processed once.

### Example

```neko
// math.neko
const saturate := (v : float) $ float {
    let mut r : float = v;
    if (r < 0.0) { r = 0.0; }
    if (r > 1.0) { r = 1.0; }
    return r;
}
```

```neko
// main.neko
import("math.neko");

@fragment
const frag := () $ neko_color : vec4 {
    let v : float = saturate(1.5);
    return vec4(v, 0.0, 0.0, 1.0);
}
```

```sh
nekoc -k main -m .
```

---

## SPIR-V Optimization

The compiler integrates SPIRV-Tools optimizer passes.

| Level (`-o`) | Effect |
|---|---|
| `0` | No optimization (default) |
| `1` | Size passes — dead code elimination, constant folding |
| `2`+ | Performance passes — aggressive inlining, loop unrolling |

Optimization runs after codegen and before validation, so the validated output is always the final optimized binary.

```sh
nekoc -k shader -o 2   # optimize for performance
nekoc -k shader -o 1   # optimize for binary size
```

---

## Complete Examples

### Passthrough vertex shader

```neko
@vertex
const vert := (pos : vec4) $ neko_position : vec4 {
    return pos;
}
```

### Solid color fragment shader

```neko
@fragment
const frag := () $ neko_color : vec4 {
    return vec4(1.0, 0.0, 0.0, 1.0);  // solid red
}
```

### Textured quad with UBO tint

```neko
@uniform(0, 0) const Material := {
    tint  : vec4,
    alpha : float
};

@sampler(0, 1) const albedo : sampler2D;

@vertex
const vert := (pos : vec2, uv : vec2) $ neko_position : vec4 {
    return vec4(pos, 0.0, 1.0);
}

@fragment
const frag := (uv : vec2) $ neko_color : vec4 {
    let tex   : vec4  = sample(albedo, uv);
    let a     : float = alpha;
    return vec4(tex, tex, tex, a);
}
```

### Helper function + control flow

```neko
const clamp01 := (v : float) $ float {
    let mut r : float = v;
    if (r < 0.0) { r = 0.0; }
    if (r > 1.0) { r = 1.0; }
    return r;
}

const sum_steps := (n : float) $ float {
    let mut acc : float = 0.0;
    let mut i   : float = 0.0;
    while (i < n) {
        acc = acc + 1.0;
        i   = i   + 1.0;
    }
    return acc;
}

@fragment
const frag := () $ neko_color : vec4 {
    let steps : float = sum_steps(4.0);   // 4.0
    let v     : float = clamp01(steps);   // 1.0
    return vec4(v, v, v, 1.0);
}
```

### Fragment built-ins

```neko
@fragment
const frag := (
    neko_frag_coord : vec4,    // gl_FragCoord — pixel position
    uv              : vec2
) $ neko_color : vec4 {
    return vec4(uv, 0.0, 1.0);
}
```

### Multi-render target

```neko
@fragment
const frag := (uv : vec2) $ neko_color0 : vec4 {
    return vec4(uv, 0.0, 1.0);
}

// Second output at Location 1:
@fragment
const frag_bright := (uv : vec2) $ neko_color1 : vec4 {
    return vec4(1.0, 1.0, 1.0, 1.0);
}
```

### Module library pattern

```neko
// lib/tonemap.neko
const reinhard := (color : vec4) $ vec4 {
    let mut r : float = color;
    r = r / (r + 1.0);
    return vec4(r, r, r, 1.0);
}
```

```neko
// shader.neko
import("tonemap.neko");

@sampler(0, 0) const hdr : sampler2D;

@fragment
const frag := (uv : vec2) $ neko_color : vec4 {
    let raw    : vec4 = sample(hdr, uv);
    return reinhard(raw);
}
```

```sh
nekoc -k shader -m lib
```

---

## Use Cases

### Vulkan rendering engine

The primary target. `nekoc` produces `.spv` files that are loaded directly via `vkCreateShaderModule`:

```cpp
std::vector<uint32_t> spirv = load_spv("shader.spv");

VkShaderModuleCreateInfo ci{};
ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
ci.codeSize = spirv.size() * 4;
ci.pCode    = spirv.data();

VkShaderModule module;
vkCreateShaderModule(device, &ci, nullptr, &module);
```

### Runtime shader compilation (C++ embedding)

Embed the `neko::Compiler` directly — compile shaders at startup or even at runtime from dynamically assembled source strings:

```cpp
neko::Compiler compiler;
neko::Options opts;
opts.validate = true;
opts.optimizationLevel = 2;
compiler.setOptions(opts);

const auto spirv = compiler.compile(shader_source);
upload_to_gpu(spirv);
```

### Shader hot-reload pipeline

Because compilation is fast (no intermediate GLSL step), NekoDSL fits naturally into a hot-reload workflow:

```cpp
// Watch .neko files for changes, recompile on save
watcher.on_change([&](const std::string& path) {
    const auto src = read_file(path);
    const auto spv = compiler.compile(src);
    pipeline.reload(spv);
});
```

### Offline shader build step

Use `nekoc` as a build step in CMake or any build system to bake `.spv` files into your release:

```cmake
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/mesh.spv
    COMMAND nekoc -k ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh -o 2
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.neko
)
```

### Shader library / standard library

Shared shader utilities (tone mapping, noise, PBR helpers) live in separate `.neko` files and are pulled in via `import`:

```
shaders/
  lib/
    math.neko
    tonemap.neko
    pbr.neko
  mesh.neko        ← import("math.neko")
  sky.neko         ← import("tonemap.neko")
```

```sh
nekoc -k mesh sky -m shaders/lib
```

---

## Limitations

| Area | Current limitation |
|---|---|
| **Types** | No matrices (`mat4`), structs, or arrays in shader body |
| **Sampling** | Only `sampler2D`; no cube maps, arrays, or 3D textures |
| **Sampling LOD** | Only implicit LOD (`OpImageSampleImplicitLod`); no `sampleLod()` |
| **Swizzle** | No `.xyz` / `.xyzw` component access; use `vec3(v, v, v)` construction instead |
| **Integers in UBOs** | `int` UBO fields are defined but untested in validation |
| **Compute shaders** | Not supported (`@compute` not implemented) |
| **Push constants** | Not supported |
| **Multiple outputs per entry point** | One output variable per entry point |
| **Tessellation / geometry stages** | Not supported |
| **Bit operations** | No `&`, `\|`, `^`, `<<`, `>>` |
| **Boolean types in interface** | `bool` is for internal comparisons only; not a valid interface type |
