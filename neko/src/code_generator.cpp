#include "neko_p.hpp"

#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neko {

// ============================================================================
// Existing helper implementations (unchanged)
// ============================================================================

uint32_t CodeGenerator::getTypeID()
{
    return getNextId();
}

uint32_t CodeGenerator::getExtInstID(const std::string& extInstName)
{
    auto it = extInsts.find(extInstName);
    if (it != extInsts.end()) return it->second;
    return extInsts[extInstName] = getNextId();
}

uint32_t CodeGenerator::registerFunction()
{
    return getNextId();
}

uint32_t CodeGenerator::getConstantID()
{
    return getNextId();
}

void CodeGenerator::registerDebugName(uint32_t resultId, const std::string& debugName)
{
    debugNames[resultId] = debugName;
}

void CodeGenerator::registerDecoration(
    uint32_t targetId,
    uint32_t decoration,
    const std::vector<std::variant<uint32_t, const char*>>& operands)
{
    decorations[targetId] = { decoration, operands };
}

void CodeGenerator::registerEntryPoint(spv::ExecutionModel /*executionModel*/, uint32_t /*function*/)
{
    // Handled by the AST-driven generate() below.
}

uint32_t CodeGenerator::getNextId()
{
    return nextId++;
}

// ============================================================================
// Anonymous helpers
// ============================================================================

namespace {

// --------------------------------------------------------------------------
// Expression traversal helpers
// --------------------------------------------------------------------------

// Collect all user-defined function names called within an expression
// (excludes built-in vec constructors).
void collect_expr_calls(const ast::Expr& expr, std::vector<std::string>& out)
{
    switch (expr.kind) {
    case ast::ExprKind::Call:
        out.push_back(expr.name);
        for (const auto& arg : expr.args) collect_expr_calls(arg, out);
        break;
    case ast::ExprKind::BinaryOp:
    case ast::ExprKind::UnaryOp:
    case ast::ExprKind::Swizzle:
        for (const auto& arg : expr.args) collect_expr_calls(arg, out);
        break;
    default: break;
    }
}

void collect_calls_block(const ast::Block& blk, std::vector<std::string>& out)
{
    for (const auto& stmt : blk.stmts) {
        if (stmt.expr.has_value()) collect_expr_calls(*stmt.expr, out);
        if (stmt.then_block) collect_calls_block(*stmt.then_block, out);
        if (stmt.else_block) collect_calls_block(*stmt.else_block, out);
    }
}

std::vector<std::string> collect_calls(const ast::FunctionDecl& func)
{
    std::vector<std::string> calls;
    collect_calls_block(func.body, calls);
    return calls;
}

// Topological sort: callees before callers.
std::vector<const ast::FunctionDecl*>
topo_order(const ast::TranslationUnit& ast)
{
    std::unordered_map<std::string, const ast::FunctionDecl*> by_name;
    for (const auto& f : ast.functions)
        by_name[f.name] = &f;

    std::unordered_set<std::string> visited;
    std::vector<const ast::FunctionDecl*> result;

    std::function<void(const std::string&)> visit = [&](const std::string& name) {
        if (visited.count(name)) return;
        visited.insert(name);
        auto it = by_name.find(name);
        if (it == by_name.end()) return;
        for (const auto& dep : collect_calls(*it->second)) visit(dep);
        result.push_back(it->second);
    };

    for (const auto& f : ast.functions) visit(f.name);
    return result;
}

// --------------------------------------------------------------------------
// Literal scanning
// --------------------------------------------------------------------------

void collect_int_literals(const ast::Expr& expr, std::unordered_set<int64_t>& out)
{
    if (expr.kind == ast::ExprKind::IntLiteral) { out.insert(expr.int_val); return; }
    for (const auto& arg : expr.args) collect_int_literals(arg, out);
}

void collect_int_literals_block(const ast::Block& blk, std::unordered_set<int64_t>& out)
{
    for (const auto& stmt : blk.stmts) {
        if (stmt.expr.has_value()) collect_int_literals(*stmt.expr, out);
        if (stmt.then_block) collect_int_literals_block(*stmt.then_block, out);
        if (stmt.else_block) collect_int_literals_block(*stmt.else_block, out);
    }
}

std::unordered_set<int64_t> all_int_literals(const ast::TranslationUnit& ast)
{
    std::unordered_set<int64_t> vals;
    for (const auto& func : ast.functions)
        collect_int_literals_block(func.body, vals);
    return vals;
}

uint32_t float_bits(double v)
{
    const float fv = static_cast<float>(v);
    uint32_t bits = 0;
    std::memcpy(&bits, &fv, sizeof(bits));
    return bits;
}

void collect_float_literals(const ast::Expr& expr, std::unordered_set<uint32_t>& out)
{
    if (expr.kind == ast::ExprKind::FloatLiteral) { out.insert(float_bits(expr.float_val)); return; }
    for (const auto& arg : expr.args) collect_float_literals(arg, out);
}

void collect_float_literals_block(const ast::Block& blk, std::unordered_set<uint32_t>& out)
{
    for (const auto& stmt : blk.stmts) {
        if (stmt.expr.has_value()) collect_float_literals(*stmt.expr, out);
        if (stmt.then_block) collect_float_literals_block(*stmt.then_block, out);
        if (stmt.else_block) collect_float_literals_block(*stmt.else_block, out);
    }
}

std::unordered_set<uint32_t> all_float_literal_bits(const ast::TranslationUnit& ast)
{
    std::unordered_set<uint32_t> bits;
    for (const auto& func : ast.functions)
        collect_float_literals_block(func.body, bits);
    return bits;
}

// --------------------------------------------------------------------------
// Type helpers
// --------------------------------------------------------------------------

bool is_vec_type(const std::string& t)
{
    return t == "vec2" || t == "vec3" || t == "vec4";
}

int vec_component_count(const std::string& t)
{
    if (t == "vec2") return 2;
    if (t == "vec3") return 3;
    if (t == "vec4") return 4;
    return 0;
}

// The SPIR-V return type for a function.
// Entry-point functions always return void in SPIR-V.
const std::string& spirv_ret(const ast::FunctionDecl& func)
{
    static const std::string void_str;
    if (!func.decorator.empty()) return void_str;
    return func.return_type;
}

// Shared void→void key for entry-points and void functions (avoids duplicate OpTypeFunction).
std::string func_type_key(const std::string& ret, const std::vector<ast::Param>& params)
{
    std::string key = (ret.empty() ? "void" : ret);
    for (const auto& p : params) { key += ':'; key += p.type; }
    return key;
}

std::string spv_func_type_key(const ast::FunctionDecl& func)
{
    if (!func.decorator.empty()) return func_type_key("", {}); // "void"
    return func_type_key(spirv_ret(func), func.params);
}

// Pointer type key: "StorageClass:TypeName"
std::string ptr_key(spv::StorageClass sc, const std::string& type_name)
{
    return std::to_string(static_cast<uint32_t>(sc)) + ":" + type_name;
}

// --------------------------------------------------------------------------
// Interface variable info
// --------------------------------------------------------------------------

struct InterfaceVarInfo {
    uint32_t          var_id;
    uint32_t          ptr_type_id;
    std::string       type_name;
    spv::StorageClass storage;
    std::string       neko_name;
    int               location   = 0; // -1 = BuiltIn decoration instead
    spv::BuiltIn      builtin_val = spv::BuiltInPosition;
    std::string       interp;    // "", "flat", "noperspective", "centroid"
};

struct EntryPointVars {
    std::vector<InterfaceVarInfo>   inputs;
    std::optional<InterfaceVarInfo> output;
};

// --------------------------------------------------------------------------
// Local variable (name → SPIR-V result ID + type)
// --------------------------------------------------------------------------

struct LocalVar {
    uint32_t    id;
    std::string type_name;
};

// Mutable local variable backed by OpVariable (Function storage class).
struct MutVar {
    uint32_t    var_id;      // ID of the OpVariable
    uint32_t    ptr_type_id; // ID of OpTypePointer Function T
    std::string type_name;   // pointee type name
};

// Pointer-type key for function-local storage.
std::string func_ptr_key(const std::string& type_name)
{
    return "fn:" + type_name;
}

// --------------------------------------------------------------------------
// Uniform buffer object helpers (std140 layout)
// --------------------------------------------------------------------------

uint32_t ubo_std140_alignment(const std::string& type)
{
    if (type == "float" || type == "int") return 4;
    if (type == "vec2")                   return 8;
    if (type == "vec3" || type == "vec4") return 16;
    return 4;
}

uint32_t ubo_std140_size(const std::string& type)
{
    if (type == "float" || type == "int") return 4;
    if (type == "vec2")                   return 8;
    if (type == "vec3")                   return 12;
    if (type == "vec4")                   return 16;
    return 4;
}

// Per-UBO SPIR-V metadata accumulated in Pass 1.
struct UboInfo {
    uint32_t struct_type_id; // OpTypeStruct
    uint32_t ptr_type_id;    // OpTypePointer Uniform → struct
    uint32_t var_id;         // OpVariable Uniform

    struct Member {
        uint32_t    index;          // position in field list
        std::string type_name;      // NekoDSL type
        uint32_t    byte_offset;    // std140 byte offset
        uint32_t    index_const_id; // OpConstant int for access chain
    };
    std::unordered_map<std::string, Member> members; // field_name → Member
};

// --------------------------------------------------------------------------
// Texture sampler metadata accumulated in Pass 1.
// --------------------------------------------------------------------------

struct SamplerInfo {
    uint32_t image_type_id;          // OpTypeImage %float 2D ...
    uint32_t sampled_image_type_id;  // OpTypeSampledImage %image_type
    uint32_t ptr_type_id;            // OpTypePointer UniformConstant %sampled_image_type
    uint32_t var_id;                 // OpVariable UniformConstant
};

} // anonymous namespace

// ============================================================================
// AST-driven SPIR-V code generation
// ============================================================================

std::vector<uint32_t> CodeGenerator::generate(const ast::TranslationUnit& ast,
                                               Options options)
{
    // =========================================================================
    // Pass 1 — ID allocation
    // =========================================================================

    // --- Type registration ---
    std::unordered_map<std::string, uint32_t> type_ids;
    type_ids["void"] = getNextId();

    auto register_type = [&](const std::string& t) {
        if (t.empty()) return;
        if (!type_ids.count(t)) type_ids[t] = getNextId();
        if (is_vec_type(t) && !type_ids.count("float"))
            type_ids["float"] = getNextId();
    };

    // Types from function signatures
    for (const auto& func : ast.functions) {
        register_type(func.return_type);
        for (const auto& p : func.params) register_type(p.type);
    }

    // Types from VarDecl explicit annotations inside function bodies (incl. nested blocks)
    std::function<void(const ast::Block&)> register_block_types = [&](const ast::Block& blk) {
        for (const auto& stmt : blk.stmts) {
            if (stmt.kind == ast::StmtKind::VarDecl && !stmt.var_type.empty())
                register_type(stmt.var_type);
            if (stmt.then_block) register_block_types(*stmt.then_block);
            if (stmt.else_block) register_block_types(*stmt.else_block);
        }
    };
    for (const auto& func : ast.functions)
        register_block_types(func.body);

    // Scalar types implied by literals
    const auto float_bits_set   = all_float_literal_bits(ast);
    const auto int_literal_vals = all_int_literals(ast);
    if (!float_bits_set.empty())   register_type("float");
    if (!int_literal_vals.empty()) register_type("int");
    // If int is in use alongside float/vec types, ensure float is registered.
    if (type_ids.count("int") &&
        (type_ids.count("float") || type_ids.count("vec2") ||
         type_ids.count("vec3") || type_ids.count("vec4")))
        register_type("float");
    // bool is needed whenever we have if/while statements (comparison results).
    auto has_branching = [](const ast::Block& b, auto& self) -> bool {
        for (const auto& s : b.stmts) {
            if (s.kind == ast::StmtKind::If || s.kind == ast::StmtKind::While) return true;
            if (s.then_block && self(*s.then_block, self)) return true;
            if (s.else_block && self(*s.else_block, self)) return true;
        }
        return false;
    };
    for (const auto& func : ast.functions)
        if (has_branching(func.body, has_branching)) { register_type("bool"); break; }

    const uint32_t void_id = type_ids.at("void");

    auto get_type_id = [&](const std::string& name) -> uint32_t {
        const std::string& key = name.empty() ? "void" : name;
        auto it = type_ids.find(key);
        if (it == type_ids.end())
            throw std::runtime_error("NekoDSL: unknown type '" + key + "'");
        return it->second;
    };

    // --- Function types ---
    std::unordered_map<std::string, uint32_t> ftype_ids;
    for (const auto& func : ast.functions) {
        const std::string key = spv_func_type_key(func);
        if (!ftype_ids.count(key)) ftype_ids[key] = getNextId();
    }

    auto get_ftype_id = [&](const ast::FunctionDecl& func) -> uint32_t {
        return ftype_ids.at(spv_func_type_key(func));
    };

    // --- Function result IDs ---
    std::unordered_map<std::string, uint32_t> func_ids;
    for (const auto& func : ast.functions)
        func_ids[func.name] = getNextId();

    // --- Constant IDs ---
    std::unordered_map<int64_t, uint32_t>  int_const_ids;
    std::unordered_map<uint32_t, uint32_t> float_const_ids;
    for (int64_t v : int_literal_vals)        int_const_ids[v]        = getNextId();
    for (uint32_t b : float_bits_set)         float_const_ids[b]       = getNextId();

    // --- Parameter IDs (non-entry-point functions only) ---
    std::unordered_map<std::string, std::vector<uint32_t>> param_ids;
    for (const auto& func : ast.functions) {
        if (!func.decorator.empty()) continue;
        auto& pids = param_ids[func.name];
        for (size_t i = 0; i < func.params.size(); ++i)
            pids.push_back(getNextId());
    }

    // --- Interface variable allocation ---
    std::unordered_map<std::string, uint32_t> ptr_type_ids;
    std::unordered_map<std::string, EntryPointVars> ep_vars;

    // Built-in input names → SPIR-V BuiltIn value.
    // Parameters with these names get BuiltIn decoration instead of Location.
    static const std::unordered_map<std::string, spv::BuiltIn> kInputBuiltins {
        { "neko_frag_coord",      spv::BuiltInFragCoord      },
        { "neko_vertex_index",    spv::BuiltInVertexIndex    },
        { "neko_instance_index",  spv::BuiltInInstanceIndex  },
        { "neko_front_facing",    spv::BuiltInFrontFacing    },
    };

    for (const auto& func : ast.functions) {
        if (func.decorator.empty()) continue;
        EntryPointVars epv;
        int input_loc = 0;

        for (const auto& param : func.params) {
            const std::string pk = ptr_key(spv::StorageClassInput, param.type);
            if (!ptr_type_ids.count(pk)) ptr_type_ids[pk] = getNextId();
            InterfaceVarInfo iv;
            iv.var_id      = getNextId();
            iv.ptr_type_id = ptr_type_ids.at(pk);
            iv.type_name   = param.type;
            iv.storage     = spv::StorageClassInput;
            iv.neko_name   = param.name;
            iv.interp      = param.interp;
            const auto bit = kInputBuiltins.find(param.name);
            if (bit != kInputBuiltins.end()) {
                iv.location    = -1;
                iv.builtin_val = bit->second;
            } else {
                iv.location = input_loc++;
            }
            epv.inputs.push_back(std::move(iv));
        }

        if (!func.output_name.empty() && !func.return_type.empty()) {
            const std::string& out_type = func.return_type;
            const std::string  pk       = ptr_key(spv::StorageClassOutput, out_type);
            if (!ptr_type_ids.count(pk)) ptr_type_ids[pk] = getNextId();
            InterfaceVarInfo ov;
            ov.var_id      = getNextId();
            ov.ptr_type_id = ptr_type_ids.at(pk);
            ov.type_name   = out_type;
            ov.storage     = spv::StorageClassOutput;
            ov.neko_name   = func.output_name;
            // Built-in outputs
            static const std::unordered_map<std::string, spv::BuiltIn> kOutputBuiltins {
                { "neko_position",  spv::BuiltInPosition  },
                { "neko_frag_depth", spv::BuiltInFragDepth },
            };
            const auto obit = kOutputBuiltins.find(func.output_name);
            if (obit != kOutputBuiltins.end()) {
                ov.location    = -1;
                ov.builtin_val = obit->second;
            } else {
                // neko_color  / neko_color0 → Location 0
                // neko_color1 → Location 1, neko_color2 → Location 2, …
                int loc = 0;
                const std::string& on = func.output_name;
                if (on.size() > 10 && on.substr(0, 10) == "neko_color") {
                    const std::string suffix = on.substr(10);
                    if (!suffix.empty() && suffix != "0")
                        loc = std::stoi(suffix);
                }
                ov.location = loc;
            }
            epv.output = std::move(ov);
        }
        ep_vars[func.name] = std::move(epv);
    }

    // --- Function-storage pointer types (for mutable locals) ---
    // Shared across all functions; keyed by "fn:<type_name>".
    std::unordered_map<std::string, uint32_t> fn_ptr_type_ids;

    // Per-function mutable variable metadata (name → MutVar).
    // We pre-scan all functions so IDs are allocated in Pass 1.
    std::unordered_map<std::string,                          // func name
                       std::unordered_map<std::string, MutVar>> // var name → MutVar
        fn_mut_vars;

    // Scan all statements recursively (including those inside if-blocks) for let mut.
    std::function<void(const ast::Block&,
                       std::unordered_map<std::string, MutVar>&)>
    collect_mut_vars = [&](const ast::Block& blk,
                           std::unordered_map<std::string, MutVar>& mut_vars) {
        for (const auto& stmt : blk.stmts) {
            if (stmt.kind == ast::StmtKind::VarDecl && stmt.is_mutable) {
                const std::string ty = stmt.var_type.empty() ? "float" : stmt.var_type;
                register_type(ty);
                const std::string pk = func_ptr_key(ty);
                if (!fn_ptr_type_ids.count(pk)) fn_ptr_type_ids[pk] = getNextId();
                MutVar mv;
                mv.var_id      = getNextId();
                mv.ptr_type_id = fn_ptr_type_ids.at(pk);
                mv.type_name   = ty;
                mut_vars[stmt.var_name] = mv;
            }
            if (stmt.then_block) collect_mut_vars(*stmt.then_block, mut_vars);
            if (stmt.else_block) collect_mut_vars(*stmt.else_block, mut_vars);
        }
    };

    for (const auto& func : ast.functions) {
        std::unordered_map<std::string, MutVar> mut_vars;
        collect_mut_vars(func.body, mut_vars);
        fn_mut_vars[func.name] = std::move(mut_vars);
    }

    // --- UBO ID allocation ---
    // Must be before emit_expr so the lambda captures these maps via [&].
    if (!ast.uniforms.empty()) register_type("int"); // needed for member-index constants

    // OpTypePointer Uniform <member_type> — deduplicated across all UBOs.
    std::unordered_map<std::string, uint32_t> ubo_member_ptr_ids;
    // Per-block metadata.
    std::unordered_map<std::string, UboInfo>  ubo_infos;

    for (const auto& ub : ast.uniforms) {
        UboInfo ui;
        ui.struct_type_id = getNextId();
        ui.ptr_type_id    = getNextId();
        ui.var_id         = getNextId();

        uint32_t byte_off = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(ub.fields.size()); ++i) {
            const auto& field = ub.fields[i];
            register_type(field.type);

            const uint32_t align = ubo_std140_alignment(field.type);
            byte_off = (byte_off + align - 1u) & ~(align - 1u);

            // Uniform member pointer type (shared across blocks).
            if (!ubo_member_ptr_ids.count(field.type))
                ubo_member_ptr_ids[field.type] = getNextId();

            // Member-index constant — reuse int_const_ids so Pass 3 emits it.
            const int64_t idx = static_cast<int64_t>(i);
            if (!int_const_ids.count(idx))
                int_const_ids[idx] = getNextId();

            UboInfo::Member m;
            m.index          = i;
            m.type_name      = field.type;
            m.byte_offset    = byte_off;
            m.index_const_id = int_const_ids.at(idx);
            ui.members[field.name] = m;

            byte_off += ubo_std140_size(field.type);
        }
        ubo_infos[ub.name] = std::move(ui);
    }

    // --- Sampler ID allocation ---
    // Shared OpTypeImage and OpTypeSampledImage (all sampler2D share the same type).
    std::unordered_map<std::string, SamplerInfo> sampler_infos;

    uint32_t image_type_id        = 0; // OpTypeImage  — allocated once if any sampler present
    uint32_t sampled_image_type_id = 0; // OpTypeSampledImage — ditto

    if (!ast.samplers.empty()) {
        register_type("float"); // required for OpTypeImage component type
        register_type("vec4");  // result type of OpImageSampleImplicitLod
        image_type_id         = getNextId();
        sampled_image_type_id = getNextId();
    }

    for (const auto& sd : ast.samplers) {
        SamplerInfo si;
        si.image_type_id         = image_type_id;
        si.sampled_image_type_id = sampled_image_type_id;
        si.ptr_type_id           = getNextId(); // OpTypePointer UniformConstant %sampled_image
        si.var_id                = getNextId(); // OpVariable UniformConstant
        sampler_infos[sd.name]   = si;
    }

    // =========================================================================
    // Pass 2 — function body emission
    // =========================================================================

    Binary func_bin;

    // emit_expr: compile an expression to SPIR-V instructions in func_bin.
    // Returns (result_id, type_name) for use by the caller.
    // 'locals'    — immutable SSA values (name → id+type)
    // 'mut_locals' — mutable OpVariable-backed vars for the current function
    std::function<std::pair<uint32_t, std::string>(
        const ast::Expr&,
        std::unordered_map<std::string, LocalVar>&,
        const std::unordered_map<std::string, MutVar>&)>
    emit_expr = [&](const ast::Expr& e,
                    std::unordered_map<std::string, LocalVar>& locals,
                    const std::unordered_map<std::string, MutVar>& mut_locals)
        -> std::pair<uint32_t, std::string>
    {
        switch (e.kind) {

        case ast::ExprKind::IntLiteral:
            return { int_const_ids.at(e.int_val), "int" };

        case ast::ExprKind::FloatLiteral:
            return { float_const_ids.at(float_bits(e.float_val)), "float" };

        case ast::ExprKind::Identifier: {
            // Check mutable vars first (they need an OpLoad at point of use).
            auto mit = mut_locals.find(e.name);
            if (mit != mut_locals.end()) {
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpLoad,
                                           get_type_id(mit->second.type_name),
                                           result_id,
                                           mit->second.var_id);
                return { result_id, mit->second.type_name };
            }
            // Check UBO fields (searched by flat field name across all uniform blocks).
            for (auto it_ubo = ubo_infos.begin(); it_ubo != ubo_infos.end(); ++it_ubo) {
                const UboInfo& ui  = it_ubo->second;
                auto fit = ui.members.find(e.name);
                if (fit == ui.members.end()) continue;
                const UboInfo::Member& mbr   = fit->second;
                const std::string      mtype = mbr.type_name;
                const uint32_t  mptr        = ubo_member_ptr_ids.at(mtype);
                const uint32_t  chain_id    = getNextId();
                const uint32_t  result_id   = getNextId();
                const uint32_t  struct_var  = ui.var_id;
                const uint32_t  idx_cid     = mbr.index_const_id;
                func_bin.writeInstruction(spv::OpAccessChain,
                                           mptr, chain_id,
                                           struct_var, idx_cid);
                func_bin.writeInstruction(spv::OpLoad,
                                           get_type_id(mtype), result_id, chain_id);
                return { result_id, mtype };
            }

            auto it = locals.find(e.name);
            if (it == locals.end())
                throw std::runtime_error(
                    "NekoDSL: undefined identifier '" + e.name + "'");
            return { it->second.id, it->second.type_name };
        }

        case ast::ExprKind::BinaryOp: {
            auto [lhs_id, lhs_ty] = emit_expr(e.args[0], locals, mut_locals);
            auto [rhs_id, rhs_ty] = emit_expr(e.args[1], locals, mut_locals);

            // Implicit coercion: int operand promoted to float when the other is float/vec.
            const bool lhs_float = (lhs_ty == "float" || is_vec_type(lhs_ty));
            const bool rhs_float = (rhs_ty == "float" || is_vec_type(rhs_ty));
            if (lhs_float && !rhs_float && rhs_ty == "int") {
                const uint32_t conv_id = getNextId();
                func_bin.writeInstruction(spv::OpConvertSToF,
                                           get_type_id("float"), conv_id, rhs_id);
                rhs_id = conv_id; rhs_ty = "float";
            } else if (rhs_float && !lhs_float && lhs_ty == "int") {
                const uint32_t conv_id = getNextId();
                func_bin.writeInstruction(spv::OpConvertSToF,
                                           get_type_id("float"), conv_id, lhs_id);
                lhs_id = conv_id; lhs_ty = "float";
            }

            // Comparison operators produce a bool result.
            static const std::unordered_set<std::string> cmp_ops
                { "==", "!=", "<", "<=", ">", ">=" };
            const bool is_cmp = cmp_ops.count(e.name) > 0;

            const std::string result_ty = is_cmp ? "bool" : lhs_ty;
            const uint32_t    type_id   = get_type_id(result_ty);
            const uint32_t    result_id = getNextId();

            spv::Op opcode;
            const bool is_float = (lhs_ty == "float" || is_vec_type(lhs_ty));
            if (is_cmp) {
                if (is_float) {
                    if      (e.name == "==") opcode = spv::OpFOrdEqual;
                    else if (e.name == "!=") opcode = spv::OpFOrdNotEqual;
                    else if (e.name == "<")  opcode = spv::OpFOrdLessThan;
                    else if (e.name == "<=") opcode = spv::OpFOrdLessThanEqual;
                    else if (e.name == ">")  opcode = spv::OpFOrdGreaterThan;
                    else                     opcode = spv::OpFOrdGreaterThanEqual;
                } else {
                    if      (e.name == "==") opcode = spv::OpIEqual;
                    else if (e.name == "!=") opcode = spv::OpINotEqual;
                    else if (e.name == "<")  opcode = spv::OpSLessThan;
                    else if (e.name == "<=") opcode = spv::OpSLessThanEqual;
                    else if (e.name == ">")  opcode = spv::OpSGreaterThan;
                    else                     opcode = spv::OpSGreaterThanEqual;
                }
            } else if (is_float) {
                if      (e.name == "+") opcode = spv::OpFAdd;
                else if (e.name == "-") opcode = spv::OpFSub;
                else if (e.name == "*") opcode = spv::OpFMul;
                else                    opcode = spv::OpFDiv;
            } else {
                if      (e.name == "+") opcode = spv::OpIAdd;
                else if (e.name == "-") opcode = spv::OpISub;
                else if (e.name == "*") opcode = spv::OpIMul;
                else                    opcode = spv::OpSDiv;
            }
            func_bin.writeInstruction(opcode, type_id, result_id, lhs_id, rhs_id);
            return { result_id, result_ty };
        }

        case ast::ExprKind::UnaryOp: {
            auto [operand_id, operand_ty] = emit_expr(e.args[0], locals, mut_locals);
            const uint32_t result_id = getNextId();
            const uint32_t type_id   = get_type_id(operand_ty);
            const bool is_float = (operand_ty == "float" || is_vec_type(operand_ty));
            const spv::Op opcode = is_float ? spv::OpFNegate : spv::OpSNegate;
            func_bin.writeInstruction(opcode, type_id, result_id, operand_id);
            return { result_id, operand_ty };
        }

        case ast::ExprKind::Call: {
            // Built-in: sample(samplerName, uv) → OpImageSampleImplicitLod → vec4
            if (e.name == "sample" && e.args.size() == 2) {
                // First arg must be an Identifier naming a declared sampler.
                if (e.args[0].kind != ast::ExprKind::Identifier)
                    throw std::runtime_error("NekoDSL: sample() first arg must be a sampler name");
                const std::string& sname = e.args[0].name;
                auto sit = sampler_infos.find(sname);
                if (sit == sampler_infos.end())
                    throw std::runtime_error("NekoDSL: unknown sampler '" + sname + "'");
                const SamplerInfo& si = sit->second;

                // Load the combined image+sampler variable.
                const uint32_t loaded_id = getNextId();
                func_bin.writeInstruction(spv::OpLoad,
                                           si.sampled_image_type_id, loaded_id, si.var_id);

                // Evaluate the UV coordinate.
                auto [uv_id, uv_ty] = emit_expr(e.args[1], locals, mut_locals);

                // Sample: OpImageSampleImplicitLod %vec4 %result %sampled_image %uv
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpImageSampleImplicitLod,
                                           get_type_id("vec4"), result_id, loaded_id, uv_id);
                return { result_id, "vec4" };
            }

            // Built-in vector constructors → OpCompositeConstruct
            // All components must be float — promote int literals automatically.
            if (is_vec_type(e.name)) {
                std::vector<uint32_t> arg_ids;
                for (const auto& arg : e.args) {
                    auto [arg_id, arg_ty] = emit_expr(arg, locals, mut_locals);
                    if (arg_ty == "int") {
                        const uint32_t conv_id = getNextId();
                        func_bin.writeInstruction(spv::OpConvertSToF,
                                                   get_type_id("float"), conv_id, arg_id);
                        arg_id = conv_id;
                    }
                    arg_ids.push_back(arg_id);
                }
                const uint32_t result_id = getNextId();
                std::vector<std::variant<uint32_t, const char*>> ops;
                ops.push_back(get_type_id(e.name));
                ops.push_back(result_id);
                for (uint32_t a : arg_ids) ops.push_back(a);
                func_bin.writeInstruction(spv::OpCompositeConstruct, ops);
                return { result_id, e.name };
            }

            // GLSL.std.450 single-argument math built-ins (float → float)
            static const std::unordered_map<std::string, uint32_t> kGlsl1 = {
                { "sin",   13 }, { "cos",   14 }, { "tan",   15 },
                { "asin",  16 }, { "acos",  17 }, { "atan",  18 },
                { "floor", 8  }, { "ceil",  9  }, { "fract", 10 },
                { "abs",   4  }, { "sqrt",  31 }, { "sign",  6  },
                { "normalize", 69 }, { "length_vec", 66 },
            };
            auto git1 = kGlsl1.find(e.name);
            if (git1 != kGlsl1.end() && e.args.size() == 1) {
                const uint32_t glsl_id = getExtInstID("GLSL.std.450");
                auto [arg_id, arg_ty] = emit_expr(e.args[0], locals, mut_locals);
                const std::string ret_ty = (e.name == "normalize") ? arg_ty : "float";
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpExtInst,
                    get_type_id(ret_ty), result_id, glsl_id,
                    git1->second, arg_id);
                return { result_id, ret_ty };
            }

            // length(vec) → float  (alias without the _vec suffix)
            if (e.name == "length" && e.args.size() == 1) {
                const uint32_t glsl_id = getExtInstID("GLSL.std.450");
                auto [arg_id, arg_ty] = emit_expr(e.args[0], locals, mut_locals);
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpExtInst,
                    get_type_id("float"), result_id, glsl_id,
                    static_cast<uint32_t>(66), arg_id);
                return { result_id, "float" };
            }

            // mix(a, b, t) → float   (GLSL FMix = 46)
            if (e.name == "mix" && e.args.size() == 3) {
                const uint32_t glsl_id = getExtInstID("GLSL.std.450");
                auto [a_id, a_ty] = emit_expr(e.args[0], locals, mut_locals);
                auto [b_id, b_ty] = emit_expr(e.args[1], locals, mut_locals);
                auto [t_id, t_ty] = emit_expr(e.args[2], locals, mut_locals);
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpExtInst,
                    get_type_id("float"), result_id, glsl_id,
                    static_cast<uint32_t>(46), a_id, b_id, t_id);
                return { result_id, "float" };
            }

            // clamp(x, lo, hi) → float   (GLSL FClamp = 43)
            if (e.name == "clamp" && e.args.size() == 3) {
                const uint32_t glsl_id = getExtInstID("GLSL.std.450");
                auto [x_id, x_ty] = emit_expr(e.args[0], locals, mut_locals);
                auto [lo_id, lo_ty] = emit_expr(e.args[1], locals, mut_locals);
                auto [hi_id, hi_ty] = emit_expr(e.args[2], locals, mut_locals);
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpExtInst,
                    get_type_id("float"), result_id, glsl_id,
                    static_cast<uint32_t>(43), x_id, lo_id, hi_id);
                return { result_id, "float" };
            }

            // dot(vec2, vec2) → float   (OpDot)
            if (e.name == "dot" && e.args.size() == 2) {
                auto [a_id, a_ty] = emit_expr(e.args[0], locals, mut_locals);
                auto [b_id, b_ty] = emit_expr(e.args[1], locals, mut_locals);
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpDot,
                    get_type_id("float"), result_id, a_id, b_id);
                return { result_id, "float" };
            }

            // pow(base, exp) → float   (GLSL Pow = 26)
            if (e.name == "pow" && e.args.size() == 2) {
                const uint32_t glsl_id = getExtInstID("GLSL.std.450");
                auto [a_id, a_ty] = emit_expr(e.args[0], locals, mut_locals);
                auto [b_id, b_ty] = emit_expr(e.args[1], locals, mut_locals);
                const uint32_t result_id = getNextId();
                func_bin.writeInstruction(spv::OpExtInst,
                    get_type_id("float"), result_id, glsl_id,
                    static_cast<uint32_t>(26), a_id, b_id);
                return { result_id, "float" };
            }

            // Regular function call
            std::vector<uint32_t> arg_result_ids;
            for (const auto& arg : e.args)
                arg_result_ids.push_back(emit_expr(arg, locals, mut_locals).first);

            auto fit = func_ids.find(e.name);
            if (fit == func_ids.end())
                throw std::runtime_error(
                    "NekoDSL: call to unknown function '" + e.name + "'");

            std::string ret_type_str = "void";
            for (const auto& f : ast.functions) {
                if (f.name == e.name) { ret_type_str = f.return_type; break; }
            }
            const uint32_t ret_type_id = get_type_id(ret_type_str);
            const uint32_t result_id   = getNextId();

            std::vector<std::variant<uint32_t, const char*>> ops;
            ops.push_back(ret_type_id);
            ops.push_back(result_id);
            ops.push_back(fit->second);
            for (uint32_t a : arg_result_ids) ops.push_back(a);
            func_bin.writeInstruction(spv::OpFunctionCall, ops);
            return { result_id, ret_type_str };
        }

        case ast::ExprKind::Swizzle: {
            // component name → index
            static const std::unordered_map<std::string, uint32_t> kIdx = {
                {"x",0},{"y",1},{"z",2},{"w",3},
                {"r",0},{"g",1},{"b",2},{"a",3},
            };
            auto [vec_id, vec_ty] = emit_expr(e.args[0], locals, mut_locals);
            const uint32_t result_id = getNextId();
            auto it = kIdx.find(e.name);
            if (it == kIdx.end())
                throw std::runtime_error("NekoDSL: unknown swizzle component '" + e.name + "'");
            func_bin.writeInstruction(spv::OpCompositeExtract,
                get_type_id("float"), result_id, vec_id, it->second);
            return { result_id, "float" };
        }

        } // switch
        throw std::runtime_error("NekoDSL: unknown expression kind");
    };

    // Emit each function in topological order (callees first).
    for (const ast::FunctionDecl* func : topo_order(ast)) {
        const bool is_entry = !func->decorator.empty();

        std::unordered_map<std::string, LocalVar> locals;
        // Mutable vars for this function (pre-allocated in Pass 1).
        const auto& mut_locals = fn_mut_vars.at(func->name);

        const std::string& spv_ret_type = spirv_ret(*func);
        const uint32_t     ret_type_id  = get_type_id(spv_ret_type);
        const uint32_t     ftype_id     = get_ftype_id(*func);
        const uint32_t     fid          = func_ids.at(func->name);

        func_bin.writeInstruction(spv::OpFunction,
                                   ret_type_id, fid,
                                   static_cast<uint32_t>(spv::FunctionControlMaskNone),
                                   ftype_id);

        if (!is_entry) {
            const auto& pids = param_ids.at(func->name);
            for (size_t i = 0; i < func->params.size(); ++i) {
                const uint32_t pid = pids[i];
                func_bin.writeInstruction(spv::OpFunctionParameter,
                                           get_type_id(func->params[i].type), pid);
                locals[func->params[i].name] = { pid, func->params[i].type };
            }
        }

        const uint32_t entry_label = getNextId();
        func_bin.writeInstruction(spv::OpLabel, entry_label);

        // OpVariable (Function) must appear immediately after the entry OpLabel.
        for (const auto& [vname, mv] : mut_locals)
            func_bin.writeInstruction(spv::OpVariable,
                                       mv.ptr_type_id, mv.var_id,
                                       static_cast<uint32_t>(spv::StorageClassFunction));

        // Entry-point: OpLoad each input interface var into a local.
        if (is_entry) {
            const auto epv_it = ep_vars.find(func->name);
            if (epv_it != ep_vars.end()) {
                for (size_t i = 0; i < func->params.size(); ++i) {
                    const auto& iv = epv_it->second.inputs[i];
                    const uint32_t lid = getNextId();
                    func_bin.writeInstruction(spv::OpLoad,
                                               get_type_id(iv.type_name), lid, iv.var_id);
                    locals[func->params[i].name] = { lid, iv.type_name };
                }
            }
        }

        // emit_stmts: process a list of statements, return true if the block
        // terminated with an explicit OpReturn / OpReturnValue.
        // Uses a Y-combinator style so the lambda can recurse into itself safely.
        // Sig: self(self, stmts) → bool terminated
        auto emit_stmts_impl = [&](auto& self,
                                   const std::vector<ast::Stmt>& stmts) -> bool {
            for (const auto& stmt : stmts) {
                switch (stmt.kind) {

                case ast::StmtKind::VarDecl: {
                    auto [val, inferred_ty] = emit_expr(*stmt.expr, locals, mut_locals);
                    const std::string ty = stmt.var_type.empty() ? inferred_ty : stmt.var_type;
                    if (stmt.is_mutable) {
                        func_bin.writeInstruction(spv::OpStore,
                                                   mut_locals.at(stmt.var_name).var_id, val);
                    } else {
                        locals[stmt.var_name] = { val, ty };
                    }
                    break;
                }

                case ast::StmtKind::Assign: {
                    auto mit = mut_locals.find(stmt.var_name);
                    if (mit == mut_locals.end())
                        throw std::runtime_error(
                            "NekoDSL: assignment to undeclared or immutable variable '"
                            + stmt.var_name + "'");
                    auto [val, val_ty] = emit_expr(*stmt.expr, locals, mut_locals);
                    func_bin.writeInstruction(spv::OpStore, mit->second.var_id, val);
                    break;
                }

                case ast::StmtKind::ExprStmt:
                    if (stmt.expr.has_value())
                        emit_expr(*stmt.expr, locals, mut_locals);
                    break;

                case ast::StmtKind::Return: {
                    if (is_entry) {
                        if (stmt.expr.has_value()) {
                            auto [val, val_ty] = emit_expr(*stmt.expr, locals, mut_locals);
                            const auto epv_it = ep_vars.find(func->name);
                            if (epv_it != ep_vars.end() && epv_it->second.output.has_value())
                                func_bin.writeInstruction(spv::OpStore,
                                                           epv_it->second.output->var_id, val);
                        }
                        func_bin.writeInstruction(spv::OpReturn);
                    } else if (spv_ret_type.empty()) {
                        if (stmt.expr.has_value()) emit_expr(*stmt.expr, locals, mut_locals);
                        func_bin.writeInstruction(spv::OpReturn);
                    } else {
                        auto [val, val_ty] = emit_expr(*stmt.expr, locals, mut_locals);
                        func_bin.writeInstruction(spv::OpReturnValue, val);
                    }
                    return true;
                }

                case ast::StmtKind::If: {
                    auto [cond_id, cond_ty] = emit_expr(*stmt.expr, locals, mut_locals);

                    const uint32_t then_label  = getNextId();
                    const uint32_t else_label  = stmt.else_block ? getNextId() : 0;
                    const uint32_t merge_label = getNextId();
                    const uint32_t branch_false =
                        stmt.else_block ? else_label : merge_label;

                    func_bin.writeInstruction(spv::OpSelectionMerge,
                                               merge_label,
                                               static_cast<uint32_t>(spv::SelectionControlMaskNone));
                    func_bin.writeInstruction(spv::OpBranchConditional,
                                               cond_id, then_label, branch_false);

                    func_bin.writeInstruction(spv::OpLabel, then_label);
                    const bool then_term = self(self, stmt.then_block->stmts);
                    if (!then_term)
                        func_bin.writeInstruction(spv::OpBranch, merge_label);

                    bool else_term = false;
                    if (stmt.else_block) {
                        func_bin.writeInstruction(spv::OpLabel, else_label);
                        else_term = self(self, stmt.else_block->stmts);
                        if (!else_term)
                            func_bin.writeInstruction(spv::OpBranch, merge_label);
                    }

                    func_bin.writeInstruction(spv::OpLabel, merge_label);
                    const bool if_term = then_term && (stmt.else_block != nullptr) && else_term;
                    if (if_term)
                        func_bin.writeInstruction(spv::OpUnreachable);
                    if (if_term) return true;
                    break;
                }
                case ast::StmtKind::While: {
                    const uint32_t header_label   = getNextId();
                    const uint32_t body_label     = getNextId();
                    const uint32_t continue_label = getNextId();
                    const uint32_t merge_label    = getNextId();

                    // Jump from the current block into the loop header.
                    func_bin.writeInstruction(spv::OpBranch, header_label);

                    // Header: evaluate the condition, then OpLoopMerge + conditional branch.
                    // (OpLoopMerge must immediately precede the terminator.)
                    func_bin.writeInstruction(spv::OpLabel, header_label);
                    auto [cond_id, cond_ty] = emit_expr(*stmt.expr, locals, mut_locals);
                    func_bin.writeInstruction(spv::OpLoopMerge, merge_label, continue_label,
                                               static_cast<uint32_t>(spv::LoopControlMaskNone));
                    func_bin.writeInstruction(spv::OpBranchConditional,
                                               cond_id, body_label, merge_label);

                    // Body block.
                    func_bin.writeInstruction(spv::OpLabel, body_label);
                    const bool body_term = self(self, stmt.then_block->stmts);
                    if (!body_term)
                        func_bin.writeInstruction(spv::OpBranch, continue_label);

                    // Continue block (back-edge to header).
                    func_bin.writeInstruction(spv::OpLabel, continue_label);
                    func_bin.writeInstruction(spv::OpBranch, header_label);

                    // Merge block (post-loop).
                    func_bin.writeInstruction(spv::OpLabel, merge_label);
                    break;
                }

                } // switch
            }
            return false;
        };

        const bool explicitly_returned = emit_stmts_impl(emit_stmts_impl, func->body.stmts);

        if (!explicitly_returned)
            func_bin.writeInstruction(spv::OpReturn);

        func_bin.writeInstruction(spv::OpFunctionEnd);
    }

    // =========================================================================
    // Pass 3 — assemble final module in SPIR-V layout order
    // =========================================================================

    const uint32_t final_bound = nextId;

    Binary bin;

    // Header
    bin.writeWords(spv::MagicNumber, spv::Version, neko::MagicNumber, final_bound, 0u);

    // Capabilities
    bin.writeInstruction(spv::OpCapability, static_cast<uint32_t>(spv::CapabilityShader));

    // ExtInstImports
    for (const auto& [name, id] : extInsts)
        bin.writeInstruction(spv::OpExtInstImport, id, name.c_str());

    // MemoryModel
    bin.writeInstruction(spv::OpMemoryModel,
                          static_cast<uint32_t>(spv::AddressingModelLogical),
                          static_cast<uint32_t>(spv::MemoryModelGLSL450));

    // OpEntryPoint (with interface variable IDs)
    for (const auto& func : ast.functions) {
        if (func.decorator.empty()) continue;
        spv::ExecutionModel model = spv::ExecutionModelVertex;
        if (func.decorator == "fragment") model = spv::ExecutionModelFragment;

        std::vector<std::variant<uint32_t, const char*>> ops;
        ops.push_back(static_cast<uint32_t>(model));
        ops.push_back(func_ids.at(func.name));
        ops.push_back(func.name.c_str());
        const auto epv_it = ep_vars.find(func.name);
        if (epv_it != ep_vars.end()) {
            for (const auto& iv : epv_it->second.inputs)  ops.push_back(iv.var_id);
            if (epv_it->second.output.has_value())         ops.push_back(epv_it->second.output->var_id);
        }
        // SPIR-V 1.6: all variables used by the entry point must be in the interface list.
        for (const auto& ub : ast.uniforms)
            ops.push_back(ubo_infos.at(ub.name).var_id);
        for (const auto& sd : ast.samplers)
            ops.push_back(sampler_infos.at(sd.name).var_id);
        bin.writeInstruction(spv::OpEntryPoint, ops);
    }

    // OpExecutionMode (Fragment requires an origin mode)
    for (const auto& func : ast.functions) {
        if (func.decorator == "fragment")
            bin.writeInstruction(spv::OpExecutionMode,
                                  func_ids.at(func.name),
                                  static_cast<uint32_t>(spv::ExecutionModeOriginUpperLeft));
    }

    // Debug
    if (options.debugInfo) {
        bin.writeInstruction(spv::OpSource,
                              static_cast<uint32_t>(neko::SourceLanguageNeko),
                              neko::Version);
        for (const auto& func : ast.functions)
            bin.writeInstruction(spv::OpName, func_ids.at(func.name), func.name.c_str());
        for (const auto& [id, name] : debugNames)
            bin.writeInstruction(spv::OpName, id, name.c_str());
    }

    // Annotations: user-registered decorations
    for (const auto& [target, deco] : decorations)
        bin.writeInstruction(spv::OpDecorate, target, deco.first, deco.second);

    // Annotations: interface variable Location / BuiltIn
    for (const auto& [fname, epv] : ep_vars) {
        for (const auto& iv : epv.inputs) {
            if (iv.location == -1)
                bin.writeInstruction(spv::OpDecorate,
                                      iv.var_id,
                                      static_cast<uint32_t>(spv::DecorationBuiltIn),
                                      static_cast<uint32_t>(iv.builtin_val));
            else
                bin.writeInstruction(spv::OpDecorate,
                                      iv.var_id,
                                      static_cast<uint32_t>(spv::DecorationLocation),
                                      static_cast<uint32_t>(iv.location));
            // Interpolation mode decorations (fragment inputs only)
            if (iv.interp == "flat")
                bin.writeInstruction(spv::OpDecorate,
                                      iv.var_id,
                                      static_cast<uint32_t>(spv::DecorationFlat));
            else if (iv.interp == "noperspective")
                bin.writeInstruction(spv::OpDecorate,
                                      iv.var_id,
                                      static_cast<uint32_t>(spv::DecorationNoPerspective));
            else if (iv.interp == "centroid")
                bin.writeInstruction(spv::OpDecorate,
                                      iv.var_id,
                                      static_cast<uint32_t>(spv::DecorationCentroid));
        }
        if (epv.output.has_value()) {
            const auto& ov = *epv.output;
            if (ov.location == -1)
                bin.writeInstruction(spv::OpDecorate,
                                      ov.var_id,
                                      static_cast<uint32_t>(spv::DecorationBuiltIn),
                                      static_cast<uint32_t>(ov.builtin_val));
            else
                bin.writeInstruction(spv::OpDecorate,
                                      ov.var_id,
                                      static_cast<uint32_t>(spv::DecorationLocation),
                                      static_cast<uint32_t>(ov.location));
        }
    }

    // Annotations: sampler descriptor set/binding
    for (const auto& sd : ast.samplers) {
        const auto& si = sampler_infos.at(sd.name);
        bin.writeInstruction(spv::OpDecorate,
                              si.var_id,
                              static_cast<uint32_t>(spv::DecorationDescriptorSet),
                              sd.set);
        bin.writeInstruction(spv::OpDecorate,
                              si.var_id,
                              static_cast<uint32_t>(spv::DecorationBinding),
                              sd.binding);
    }

    // Annotations: UBO Block decoration, member offsets, descriptor set/binding
    for (const auto& ub : ast.uniforms) {
        const auto& ui = ubo_infos.at(ub.name);
        bin.writeInstruction(spv::OpDecorate,
                              ui.struct_type_id,
                              static_cast<uint32_t>(spv::DecorationBlock));
        for (const auto& field : ub.fields) {
            const auto& m = ui.members.at(field.name);
            bin.writeInstruction(spv::OpMemberDecorate,
                                  ui.struct_type_id, m.index,
                                  static_cast<uint32_t>(spv::DecorationOffset),
                                  m.byte_offset);
        }
        bin.writeInstruction(spv::OpDecorate,
                              ui.var_id,
                              static_cast<uint32_t>(spv::DecorationDescriptorSet),
                              ub.set);
        bin.writeInstruction(spv::OpDecorate,
                              ui.var_id,
                              static_cast<uint32_t>(spv::DecorationBinding),
                              ub.binding);
    }

    // Types: void
    bin.writeInstruction(spv::OpTypeVoid, void_id);

    // Types: scalars (float before vec)
    for (const char* name : {"float", "int", "bool"}) {
        auto it = type_ids.find(name);
        if (it == type_ids.end()) continue;
        if      (std::string(name) == "float") bin.writeInstruction(spv::OpTypeFloat, it->second, 32u);
        else if (std::string(name) == "int")   bin.writeInstruction(spv::OpTypeInt,   it->second, 32u, 1u);
        else                                   bin.writeInstruction(spv::OpTypeBool,  it->second);
    }

    // Types: vectors
    for (const char* vn : {"vec2", "vec3", "vec4"}) {
        auto it = type_ids.find(vn);
        if (it == type_ids.end()) continue;
        bin.writeInstruction(spv::OpTypeVector,
                              it->second,
                              type_ids.at("float"),
                              static_cast<uint32_t>(vec_component_count(vn)));
    }

    // Types: sampler image/sampledImage types (shared across all sampler2D decls)
    if (!ast.samplers.empty()) {
        // OpTypeImage %float 2D 0 0 0 1 Unknown
        // Operands: sampled_type depth arrayed ms sampled format
        bin.writeInstruction(spv::OpTypeImage,
                              image_type_id,
                              type_ids.at("float"),        // sampled component type
                              static_cast<uint32_t>(spv::Dim2D),
                              0u,  // depth = 0 (not a depth image)
                              0u,  // arrayed = 0
                              0u,  // multisampled = 0
                              1u,  // sampled = 1 (used with a sampler)
                              static_cast<uint32_t>(spv::ImageFormatUnknown));
        bin.writeInstruction(spv::OpTypeSampledImage,
                              sampled_image_type_id, image_type_id);
        // Per-sampler pointer type and variable
        for (const auto& sd : ast.samplers) {
            const auto& si = sampler_infos.at(sd.name);
            bin.writeInstruction(spv::OpTypePointer,
                                  si.ptr_type_id,
                                  static_cast<uint32_t>(spv::StorageClassUniformConstant),
                                  si.sampled_image_type_id);
        }
    }

    // Types: UBO structs and their pointer types
    for (const auto& ub : ast.uniforms) {
        const auto& ui = ubo_infos.at(ub.name);
        std::vector<std::variant<uint32_t, const char*>> ops;
        ops.push_back(ui.struct_type_id);
        for (const auto& field : ub.fields)
            ops.push_back(get_type_id(field.type));
        bin.writeInstruction(spv::OpTypeStruct, ops);
        bin.writeInstruction(spv::OpTypePointer,
                              ui.ptr_type_id,
                              static_cast<uint32_t>(spv::StorageClassUniform),
                              ui.struct_type_id);
    }
    // OpTypePointer Uniform <member_type> — one per distinct member type
    for (const auto& [type_name, ptid] : ubo_member_ptr_ids)
        bin.writeInstruction(spv::OpTypePointer,
                              ptid,
                              static_cast<uint32_t>(spv::StorageClassUniform),
                              get_type_id(type_name));

    // Types: pointer types for interface variables (Input / Output storage)
    for (const auto& [pk, ptid] : ptr_type_ids) {
        const size_t      col       = pk.find(':');
        const uint32_t    sc        = static_cast<uint32_t>(std::stoul(pk.substr(0, col)));
        const std::string type_name = pk.substr(col + 1);
        bin.writeInstruction(spv::OpTypePointer, ptid, sc, get_type_id(type_name));
    }

    // Types: pointer types for mutable function-local variables (Function storage)
    for (const auto& [pk, ptid] : fn_ptr_type_ids) {
        // pk is "fn:<type_name>"
        const std::string type_name = pk.substr(3);
        bin.writeInstruction(spv::OpTypePointer, ptid,
                              static_cast<uint32_t>(spv::StorageClassFunction),
                              get_type_id(type_name));
    }

    // Types: function types
    for (const auto& [key, tid] : ftype_ids) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : key) {
            if (c == ':') { parts.push_back(cur); cur.clear(); }
            else          { cur += c; }
        }
        parts.push_back(cur);

        const uint32_t ret_id =
            (parts[0] == "void") ? void_id : type_ids.at(parts[0]);

        std::vector<std::variant<uint32_t, const char*>> ops;
        ops.push_back(tid);
        ops.push_back(ret_id);
        for (size_t i = 1; i < parts.size(); ++i)
            if (!parts[i].empty()) ops.push_back(type_ids.at(parts[i]));

        bin.writeInstruction(spv::OpTypeFunction, ops);
    }

    // Constants: integers
    for (const auto& [val, cid] : int_const_ids)
        bin.writeInstruction(spv::OpConstant,
                              type_ids.at("int"), cid,
                              static_cast<uint32_t>(val));

    // Constants: floats
    for (const auto& [bits, cid] : float_const_ids)
        bin.writeInstruction(spv::OpConstant,
                              type_ids.at("float"), cid, bits);

    // Global variables (interface)
    for (const auto& [fname, epv] : ep_vars) {
        for (const auto& iv : epv.inputs)
            bin.writeInstruction(spv::OpVariable,
                                  iv.ptr_type_id, iv.var_id,
                                  static_cast<uint32_t>(iv.storage));
        if (epv.output.has_value())
            bin.writeInstruction(spv::OpVariable,
                                  epv.output->ptr_type_id, epv.output->var_id,
                                  static_cast<uint32_t>(epv.output->storage));
    }

    // UBO global variables
    for (const auto& ub : ast.uniforms) {
        const auto& ui = ubo_infos.at(ub.name);
        bin.writeInstruction(spv::OpVariable,
                              ui.ptr_type_id, ui.var_id,
                              static_cast<uint32_t>(spv::StorageClassUniform));
    }

    // Sampler global variables (UniformConstant storage class)
    for (const auto& sd : ast.samplers) {
        const auto& si = sampler_infos.at(sd.name);
        bin.writeInstruction(spv::OpVariable,
                              si.ptr_type_id, si.var_id,
                              static_cast<uint32_t>(spv::StorageClassUniformConstant));
    }

    // Function bodies
    for (uint32_t word : func_bin.get())
        bin.writeWord(word);

    return bin.get();
}

} // namespace neko
