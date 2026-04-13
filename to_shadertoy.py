#!/usr/bin/env python3
"""
to_shadertoy.py — converts a NekoDSL .spv fragment shader to Shadertoy-ready GLSL.

Usage:
    python to_shadertoy.py <file.spv>
"""

import subprocess, sys, re, os

SPIRV_CROSS = r"dependencies\SPIRV-Cross\build\spirv-cross.exe"

def to_shadertoy(spv_file):
    result = subprocess.run(
        [SPIRV_CROSS, spv_file, "--vulkan-semantics", "--entry", "frag", "--stage", "frag"],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        sys.exit(f"spirv-cross error:\n{result.stderr}")

    glsl = result.stdout

    # ── 1. Collect names before stripping ─────────────────────────────────────

    # Output variable: layout(location=0) out vec4 _XX
    out_var = None
    m = re.search(r'layout\([^)]*\)\s+out\s+\w+\s+(\w+)\s*;', glsl)
    if m:
        out_var = m.group(1)

    # Input vec2 variables (UV coordinates from vertex shader)
    # layout(location=...) [qualifier] in vec2 _XX
    uv_vars = []
    for m in re.finditer(r'layout\([^)]*\)\s+(?:\w+\s+)*in\s+vec2\s+(\w+)\s*;', glsl):
        uv_vars.append(m.group(1))

    # UBO float fields: layout(...) uniform STRUCT { ... } VAR;
    # Map each field reference "VAR._mN" → animated iTime expression
    ubo_replacements = {}
    for m in re.finditer(
        r'layout\([^)]*\)\s+uniform\s+\w+\s*\n\{([^}]*)\}\s*(\w+)\s*;', glsl
    ):
        body, var_name = m.group(1), m.group(2)
        # Members are named _m0, _m1, ... in order
        field_idx = 0
        for fm in re.finditer(r'(float|vec(\d))\s+\w+\s*;', body):
            ftype, dim = fm.group(1), fm.group(2)
            key = f"{var_name}._m{field_idx}"
            if ftype == "float":
                ubo_replacements[key] = "iTime"
            elif dim:
                ubo_replacements[key] = f"vec{dim}(iTime)"
            field_idx += 1

    # Samplers: layout(...) uniform samplerXX _XX
    sampler_replacements = {}
    for idx, m in enumerate(
        re.finditer(r'layout\([^)]*\)\s+uniform\s+sampler\w+\s+(\w+)\s*;', glsl)
    ):
        sampler_replacements[m.group(1)] = f"iChannel{idx}"

    # ── 2. Strip Vulkan-specific declarations ──────────────────────────────────

    # Remove multi-line UBO blocks
    glsl = re.sub(
        r'layout\([^)]*\)\s+uniform\s+\w+\s*\n\{[^}]*\}\s*\w+\s*;',
        '', glsl
    )
    # Remove single-line layout declarations (sampler, in, out)
    glsl = re.sub(r'^layout\([^)]*\).*;\n', '', glsl, flags=re.MULTILINE)
    # Remove #version
    glsl = re.sub(r'^#version.*\n', '', glsl, flags=re.MULTILINE)
    # Clean up leading blank lines
    glsl = glsl.lstrip('\n')

    # ── 3. Apply replacements ──────────────────────────────────────────────────

    # void main() → mainImage signature
    glsl = re.sub(
        r'\bvoid\s+main\s*\(\s*\)',
        'void mainImage(out vec4 fragColor, in vec2 fragCoord)',
        glsl
    )

    # Output variable → fragColor
    if out_var:
        glsl = re.sub(rf'\b{re.escape(out_var)}\b', 'fragColor', glsl)

    # UBO fields → iTime (longest keys first to avoid partial replacements)
    for key in sorted(ubo_replacements, key=len, reverse=True):
        glsl = glsl.replace(key, ubo_replacements[key])

    # Samplers → iChannel0, iChannel1, ...
    for var, channel in sampler_replacements.items():
        glsl = re.sub(rf'\b{re.escape(var)}\b', channel, glsl)

    # ── 4. Inject UV variable declarations inside mainImage ───────────────────
    # Each in vec2 _XX becomes: vec2 _XX = fragCoord / iResolution.xy;
    if uv_vars:
        uv_decls = "\n".join(
            f"    vec2 {v} = fragCoord / iResolution.xy;" for v in uv_vars
        )
        glsl = re.sub(
            r'(void mainImage\([^)]*\)\s*\{)',
            r'\1\n' + uv_decls,
            glsl
        )

    return glsl.strip()

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(f"Usage: python {os.path.basename(sys.argv[0])} <file.spv>")
    print(to_shadertoy(sys.argv[1]))
