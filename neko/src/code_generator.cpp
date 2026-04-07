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

// Collect all function names directly called within an expression.
void collect_expr_calls(const ast::Expr& expr, std::vector<std::string>& out)
{
    if (expr.kind == ast::ExprKind::Call) {
        out.push_back(expr.name);
        for (const auto& arg : expr.args)
            collect_expr_calls(arg, out);
    }
}

// All function names called anywhere in a function's body.
std::vector<std::string> collect_calls(const ast::FunctionDecl& func)
{
    std::vector<std::string> calls;
    for (const auto& stmt : func.body.stmts)
        if (stmt.expr.has_value())
            collect_expr_calls(*stmt.expr, calls);
    return calls;
}

// Topological sort: return functions in dependency-first order (callees before callers).
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
        if (it == by_name.end()) return; // external / undefined — skip
        for (const auto& dep : collect_calls(*it->second))
            visit(dep);
        result.push_back(it->second);
    };

    for (const auto& f : ast.functions)
        visit(f.name);

    return result;
}

// Scan all integer literals in an expression tree.
void collect_int_literals(const ast::Expr& expr, std::unordered_set<int64_t>& out)
{
    if (expr.kind == ast::ExprKind::IntLiteral)  out.insert(expr.int_val);
    if (expr.kind == ast::ExprKind::Call)
        for (const auto& arg : expr.args)
            collect_int_literals(arg, out);
}

// All unique integer literals used anywhere in the AST.
std::unordered_set<int64_t> all_int_literals(const ast::TranslationUnit& ast)
{
    std::unordered_set<int64_t> vals;
    for (const auto& func : ast.functions)
        for (const auto& stmt : func.body.stmts)
            if (stmt.expr.has_value())
                collect_int_literals(*stmt.expr, vals);
    return vals;
}

// The SPIR-V return type for a function.
// Entry-point functions (with a decorator) must return void in SPIR-V.
const std::string& spirv_ret(const ast::FunctionDecl& func)
{
    static const std::string void_str; // empty == void
    if (!func.decorator.empty()) return void_str;
    return func.return_type;
}

// Build a string key for a function type: "ret:param0:param1:…"
std::string func_type_key(const std::string& ret,
                           const std::vector<ast::Param>& params)
{
    std::string key = (ret.empty() ? "void" : ret);
    for (const auto& p : params) { key += ':'; key += p.type; }
    return key;
}

// --------------------------------------------------------------------------
// Phase 2 helpers
// --------------------------------------------------------------------------

// True for built-in vector types.
bool is_vec_type(const std::string& t)
{
    return t == "vec2" || t == "vec3" || t == "vec4";
}

// Component count for a vector type.
int vec_component_count(const std::string& t)
{
    if (t == "vec2") return 2;
    if (t == "vec3") return 3;
    if (t == "vec4") return 4;
    return 0;
}

// IEEE 754 single-precision bit pattern of a double value.
uint32_t float_bits(double v)
{
    const float fv = static_cast<float>(v);
    uint32_t bits = 0;
    std::memcpy(&bits, &fv, sizeof(bits));
    return bits;
}

// Collect all float literal bit patterns in an expression tree.
void collect_float_literals(const ast::Expr& expr, std::unordered_set<uint32_t>& out)
{
    if (expr.kind == ast::ExprKind::FloatLiteral)
        out.insert(float_bits(expr.float_val));
    if (expr.kind == ast::ExprKind::Call)
        for (const auto& arg : expr.args)
            collect_float_literals(arg, out);
}

// All unique float literal bit patterns across the whole AST.
std::unordered_set<uint32_t> all_float_literal_bits(const ast::TranslationUnit& ast)
{
    std::unordered_set<uint32_t> bits;
    for (const auto& func : ast.functions)
        for (const auto& stmt : func.body.stmts)
            if (stmt.expr.has_value())
                collect_float_literals(*stmt.expr, bits);
    return bits;
}

// SPIR-V function type key.
// Entry-point functions are void→void in SPIR-V (params go via interface vars).
// Use func_type_key("", {}) so the key is "void" — consistent with non-entry
// void functions, allowing them to share the same OpTypeFunction declaration.
std::string spv_func_type_key(const ast::FunctionDecl& func)
{
    if (!func.decorator.empty())
        return func_type_key("", {}); // "void" — void return, no SPIR-V params
    return func_type_key(spirv_ret(func), func.params);
}

// Key for a pointer type: "StorageClass:TypeName"
std::string ptr_key(spv::StorageClass sc, const std::string& type_name)
{
    return std::to_string(static_cast<uint32_t>(sc)) + ":" + type_name;
}

// Information for a single Input or Output interface variable.
struct InterfaceVarInfo {
    uint32_t          var_id;
    uint32_t          ptr_type_id;
    std::string       type_name;
    spv::StorageClass storage;
    std::string       neko_name;     // original param / output name
    int               location = 0; // ≥ 0 = Location decoration; -1 = BuiltIn
    spv::BuiltIn      builtin_val = spv::BuiltInPosition;
};

// Interface variable collection for one entry-point function.
struct EntryPointVars {
    std::vector<InterfaceVarInfo>  inputs;
    std::optional<InterfaceVarInfo> output;
};

} // anonymous namespace

// ============================================================================
// AST-driven SPIR-V code generation
// https://www.khronos.org/registry/spir-v/specs/unified1/SPIRV.html
// ============================================================================

std::vector<uint32_t> CodeGenerator::generate(const ast::TranslationUnit& ast,
                                               Options options)
{
    // =========================================================================
    // Pass 1 — ID allocation
    // All SPIR-V IDs must be allocated before the header (which encodes Bound).
    // =========================================================================

    // --- Type registration helper ---
    std::unordered_map<std::string, uint32_t> type_ids;
    type_ids["void"] = getNextId();

    // Registers a type name and ensures its dependencies are also registered.
    // vec types depend on "float" being present.
    auto register_type = [&](const std::string& t) {
        if (t.empty()) return;
        if (!type_ids.count(t))
            type_ids[t] = getNextId();
        if (is_vec_type(t) && !type_ids.count("float"))
            type_ids["float"] = getNextId();
    };

    // Collect types from function signatures (return types, param types, output types)
    for (const auto& func : ast.functions) {
        register_type(func.return_type);
        for (const auto& p : func.params)
            register_type(p.type);
    }

    // Register scalar types implied by literal values
    const auto float_bits_set  = all_float_literal_bits(ast);
    const auto int_literal_vals = all_int_literals(ast);
    if (!float_bits_set.empty())   register_type("float");
    if (!int_literal_vals.empty()) register_type("int");

    const uint32_t void_id = type_ids.at("void");

    auto get_type_id = [&](const std::string& name) -> uint32_t {
        const std::string& key = name.empty() ? "void" : name;
        auto it = type_ids.find(key);
        if (it == type_ids.end())
            throw std::runtime_error("NekoDSL: unknown type '" + key + "'");
        return it->second;
    };

    // --- Function types (keyed by "ret:p0:p1:…") ---
    std::unordered_map<std::string, uint32_t> ftype_ids;
    for (const auto& func : ast.functions) {
        const std::string key = spv_func_type_key(func);
        if (!ftype_ids.count(key))
            ftype_ids[key] = getNextId();
    }

    auto get_ftype_id = [&](const ast::FunctionDecl& func) -> uint32_t {
        return ftype_ids.at(spv_func_type_key(func));
    };

    // --- Function result IDs ---
    std::unordered_map<std::string, uint32_t> func_ids;
    for (const auto& func : ast.functions)
        func_ids[func.name] = getNextId();

    // --- Integer constant IDs ---
    std::unordered_map<int64_t, uint32_t> int_const_ids;
    for (int64_t v : int_literal_vals)
        int_const_ids[v] = getNextId();

    // --- Float constant IDs (keyed by single-precision bit pattern) ---
    std::unordered_map<uint32_t, uint32_t> float_const_ids;
    for (uint32_t bits : float_bits_set)
        float_const_ids[bits] = getNextId();

    // --- Parameter IDs for NON-entry-point functions only ---
    // Entry-point parameters become interface variables (global OpVariable).
    std::unordered_map<std::string, std::vector<uint32_t>> param_ids;
    for (const auto& func : ast.functions) {
        if (!func.decorator.empty()) continue;
        auto& pids = param_ids[func.name];
        for (size_t i = 0; i < func.params.size(); ++i)
            pids.push_back(getNextId());
    }

    // --- Interface variable allocation for entry-point functions ---
    // Pointer types are shared across functions (same StorageClass + pointee type).
    std::unordered_map<std::string, uint32_t> ptr_type_ids; // ptr_key → ID
    std::unordered_map<std::string, EntryPointVars> ep_vars; // func name → vars

    for (const auto& func : ast.functions) {
        if (func.decorator.empty()) continue;
        EntryPointVars epv;
        int input_loc = 0;

        // One Input variable per parameter
        for (const auto& param : func.params) {
            const std::string pk = ptr_key(spv::StorageClassInput, param.type);
            if (!ptr_type_ids.count(pk))
                ptr_type_ids[pk] = getNextId();

            InterfaceVarInfo iv;
            iv.var_id      = getNextId();
            iv.ptr_type_id = ptr_type_ids.at(pk);
            iv.type_name   = param.type;
            iv.storage     = spv::StorageClassInput;
            iv.neko_name   = param.name;
            iv.location    = input_loc++;
            epv.inputs.push_back(std::move(iv));
        }

        // One Output variable for the named output (if any)
        if (!func.output_name.empty() && !func.return_type.empty()) {
            const std::string& out_type = func.return_type;
            const std::string  pk       = ptr_key(spv::StorageClassOutput, out_type);
            if (!ptr_type_ids.count(pk))
                ptr_type_ids[pk] = getNextId();

            InterfaceVarInfo ov;
            ov.var_id      = getNextId();
            ov.ptr_type_id = ptr_type_ids.at(pk);
            ov.type_name   = out_type;
            ov.storage     = spv::StorageClassOutput;
            ov.neko_name   = func.output_name;
            if (func.output_name == "neko_position") {
                ov.location   = -1; // BuiltIn, not Location
                ov.builtin_val = spv::BuiltInPosition;
            } else {
                ov.location = 0; // default output attachment
            }
            epv.output = std::move(ov);
        }

        ep_vars[func.name] = std::move(epv);
    }

    // =========================================================================
    // Pass 2 — function body emission
    // Generate OpFunction … OpFunctionEnd blocks into a scratch Binary.
    // Label IDs and call-result IDs are allocated here.
    // =========================================================================

    Binary func_bin;

    // Resolve expression → SPIR-V result ID, emitting instructions into func_bin.
    std::function<uint32_t(const ast::Expr&,
                            const std::unordered_map<std::string, uint32_t>&)>
    emit_expr = [&](const ast::Expr& e,
                    const std::unordered_map<std::string, uint32_t>& locals)
        -> uint32_t
    {
        switch (e.kind) {

        case ast::ExprKind::IntLiteral:
            return int_const_ids.at(e.int_val);

        case ast::ExprKind::FloatLiteral:
            return float_const_ids.at(float_bits(e.float_val));

        case ast::ExprKind::Identifier: {
            auto it = locals.find(e.name);
            if (it == locals.end())
                throw std::runtime_error(
                    "NekoDSL: undefined identifier '" + e.name + "'");
            return it->second;
        }

        case ast::ExprKind::Call: {
            // Vector constructors are built-in; use OpCompositeConstruct.
            if (is_vec_type(e.name)) {
                std::vector<uint32_t> arg_ids;
                for (const auto& arg : e.args)
                    arg_ids.push_back(emit_expr(arg, locals));

                const uint32_t result_id = getNextId();
                std::vector<std::variant<uint32_t, const char*>> ops;
                ops.push_back(get_type_id(e.name)); // ResultType
                ops.push_back(result_id);            // Result
                for (uint32_t a : arg_ids) ops.push_back(a);
                func_bin.writeInstruction(spv::OpCompositeConstruct, ops);
                return result_id;
            }

            // Regular function call
            std::vector<uint32_t> arg_result_ids;
            for (const auto& arg : e.args)
                arg_result_ids.push_back(emit_expr(arg, locals));

            auto fit = func_ids.find(e.name);
            if (fit == func_ids.end())
                throw std::runtime_error(
                    "NekoDSL: call to unknown function '" + e.name + "'");

            // Callee NekoDSL return type (not the SPIR-V entry-point override).
            uint32_t ret_type_id = void_id;
            for (const auto& f : ast.functions) {
                if (f.name == e.name) {
                    ret_type_id = get_type_id(f.return_type);
                    break;
                }
            }

            const uint32_t result_id = getNextId();
            std::vector<std::variant<uint32_t, const char*>> ops;
            ops.push_back(ret_type_id);
            ops.push_back(result_id);
            ops.push_back(fit->second);
            for (uint32_t a : arg_result_ids) ops.push_back(a);
            func_bin.writeInstruction(spv::OpFunctionCall, ops);
            return result_id;
        }
        } // switch
        throw std::runtime_error("NekoDSL: unknown expression kind");
    };

    // Emit each function in dependency order (callees before callers).
    for (const ast::FunctionDecl* func : topo_order(ast)) {
        const bool is_entry = !func->decorator.empty();

        std::unordered_map<std::string, uint32_t> locals;

        const std::string& spv_ret_type = spirv_ret(*func);
        const uint32_t     ret_type_id  = get_type_id(spv_ret_type);
        const uint32_t     ftype_id     = get_ftype_id(*func);
        const uint32_t     fid          = func_ids.at(func->name);

        // OpFunction ResultType ResultId FunctionControl FunctionType
        func_bin.writeInstruction(spv::OpFunction,
                                   ret_type_id,
                                   fid,
                                   static_cast<uint32_t>(spv::FunctionControlMaskNone),
                                   ftype_id);

        if (!is_entry) {
            // Non-entry-point: emit OpFunctionParameter for each param.
            const auto& pids = param_ids.at(func->name);
            for (size_t i = 0; i < func->params.size(); ++i) {
                func_bin.writeInstruction(spv::OpFunctionParameter,
                                           get_type_id(func->params[i].type),
                                           pids[i]);
                locals[func->params[i].name] = pids[i];
            }
        }
        // Entry-point: no OpFunctionParameter — params come via interface vars.

        // Entry block label
        const uint32_t entry_label = getNextId();
        func_bin.writeInstruction(spv::OpLabel, entry_label);

        // Entry-point: OpLoad each input interface variable into a local.
        if (is_entry && ep_vars.count(func->name)) {
            const auto& epv = ep_vars.at(func->name);
            for (size_t i = 0; i < func->params.size(); ++i) {
                const auto& iv     = epv.inputs[i];
                const uint32_t lid = getNextId();
                func_bin.writeInstruction(spv::OpLoad,
                                           get_type_id(iv.type_name),
                                           lid,
                                           iv.var_id);
                locals[func->params[i].name] = lid;
            }
        }

        // Statements
        bool explicitly_returned = false;
        for (const auto& stmt : func->body.stmts) {
            if (stmt.kind == ast::StmtKind::ExprStmt) {
                if (stmt.expr.has_value())
                    emit_expr(*stmt.expr, locals); // result discarded
            } else { // Return
                if (is_entry) {
                    // Entry-point return: evaluate, store to output var (if any), OpReturn.
                    if (stmt.expr.has_value()) {
                        const uint32_t val = emit_expr(*stmt.expr, locals);
                        const auto epv_it = ep_vars.find(func->name);
                        if (epv_it != ep_vars.end() &&
                            epv_it->second.output.has_value()) {
                            func_bin.writeInstruction(spv::OpStore,
                                                       epv_it->second.output->var_id,
                                                       val);
                        }
                    }
                    func_bin.writeInstruction(spv::OpReturn);
                } else if (spv_ret_type.empty()) {
                    // Void non-entry-point: evaluate for side effects, OpReturn.
                    if (stmt.expr.has_value())
                        emit_expr(*stmt.expr, locals);
                    func_bin.writeInstruction(spv::OpReturn);
                } else {
                    // Non-void non-entry-point: OpReturnValue.
                    const uint32_t val = emit_expr(*stmt.expr, locals);
                    func_bin.writeInstruction(spv::OpReturnValue, val);
                }
                explicitly_returned = true;
            }
        }

        if (!explicitly_returned)
            func_bin.writeInstruction(spv::OpReturn);

        func_bin.writeInstruction(spv::OpFunctionEnd);
    }

    // =========================================================================
    // Pass 3 — assemble final module in SPIR-V logical layout order
    // =========================================================================

    const uint32_t final_bound = nextId; // all IDs have now been allocated

    Binary bin;

    // ----- Header (5 words) -----
    bin.writeWords(spv::MagicNumber,   // Magic
                   spv::Version,       // SPIR-V version
                   neko::MagicNumber,  // Generator
                   final_bound,        // Bound (max ID + 1)
                   0u);                // Reserved schema word

    // ----- Capabilities -----
    bin.writeInstruction(spv::OpCapability,
                          static_cast<uint32_t>(spv::CapabilityShader));

    // ----- ExtInstImports -----
    for (const auto& [name, id] : extInsts)
        bin.writeInstruction(spv::OpExtInstImport, id, name.c_str());

    // ----- MemoryModel -----
    bin.writeInstruction(spv::OpMemoryModel,
                          static_cast<uint32_t>(spv::AddressingModelLogical),
                          static_cast<uint32_t>(spv::MemoryModelGLSL450));

    // ----- OpEntryPoint (with interface variable IDs) -----
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
            for (const auto& iv : epv_it->second.inputs)
                ops.push_back(iv.var_id);
            if (epv_it->second.output.has_value())
                ops.push_back(epv_it->second.output->var_id);
        }
        bin.writeInstruction(spv::OpEntryPoint, ops);
    }

    // ----- OpExecutionMode (Fragment requires an origin mode) -----
    for (const auto& func : ast.functions) {
        if (func.decorator == "fragment") {
            bin.writeInstruction(spv::OpExecutionMode,
                                  func_ids.at(func.name),
                                  static_cast<uint32_t>(spv::ExecutionModeOriginUpperLeft));
        }
    }

    // ----- Debug (OpSource / OpName) -----
    if (options.debugInfo) {
        bin.writeInstruction(spv::OpSource,
                              static_cast<uint32_t>(neko::SourceLanguageNeko),
                              neko::Version);
        for (const auto& func : ast.functions)
            bin.writeInstruction(spv::OpName,
                                  func_ids.at(func.name),
                                  func.name.c_str());
        for (const auto& [id, name] : debugNames)
            bin.writeInstruction(spv::OpName, id, name.c_str());
    }

    // ----- Annotations (OpDecorate) -----
    // Existing user-registered decorations
    for (const auto& [target, deco] : decorations)
        bin.writeInstruction(spv::OpDecorate,
                              target,
                              deco.first,
                              deco.second);

    // Interface variable decorations (Location / BuiltIn)
    for (const auto& [fname, epv] : ep_vars) {
        for (const auto& iv : epv.inputs) {
            bin.writeInstruction(spv::OpDecorate,
                                  iv.var_id,
                                  static_cast<uint32_t>(spv::DecorationLocation),
                                  static_cast<uint32_t>(iv.location));
        }
        if (epv.output.has_value()) {
            const auto& ov = *epv.output;
            if (ov.location == -1) {
                bin.writeInstruction(spv::OpDecorate,
                                      ov.var_id,
                                      static_cast<uint32_t>(spv::DecorationBuiltIn),
                                      static_cast<uint32_t>(ov.builtin_val));
            } else {
                bin.writeInstruction(spv::OpDecorate,
                                      ov.var_id,
                                      static_cast<uint32_t>(spv::DecorationLocation),
                                      static_cast<uint32_t>(ov.location));
            }
        }
    }

    // ----- Type declarations (dependency order) -----

    // 1. void
    bin.writeInstruction(spv::OpTypeVoid, void_id);

    // 2. Scalar types (float must precede vec types)
    for (const char* name : {"float", "int", "bool"}) {
        auto it = type_ids.find(name);
        if (it == type_ids.end()) continue;
        if      (std::string(name) == "float") bin.writeInstruction(spv::OpTypeFloat, it->second, 32u);
        else if (std::string(name) == "int")   bin.writeInstruction(spv::OpTypeInt,   it->second, 32u, 1u);
        else                                   bin.writeInstruction(spv::OpTypeBool,  it->second);
    }

    // 3. Vector types (require float already declared)
    for (const char* vn : {"vec2", "vec3", "vec4"}) {
        auto it = type_ids.find(vn);
        if (it == type_ids.end()) continue;
        bin.writeInstruction(spv::OpTypeVector,
                              it->second,
                              type_ids.at("float"),
                              static_cast<uint32_t>(vec_component_count(vn)));
    }

    // 4. Pointer types for interface variables
    for (const auto& [pk, ptid] : ptr_type_ids) {
        const size_t col = pk.find(':');
        const uint32_t    sc        = static_cast<uint32_t>(std::stoul(pk.substr(0, col)));
        const std::string type_name = pk.substr(col + 1);
        bin.writeInstruction(spv::OpTypePointer, ptid, sc, get_type_id(type_name));
    }

    // 5. Function type declarations
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
            if (!parts[i].empty())
                ops.push_back(type_ids.at(parts[i]));

        bin.writeInstruction(spv::OpTypeFunction, ops);
    }

    // ----- Constant declarations -----

    // Integer constants
    for (const auto& [val, cid] : int_const_ids)
        bin.writeInstruction(spv::OpConstant,
                              type_ids.at("int"),
                              cid,
                              static_cast<uint32_t>(val));

    // Float constants (emitted as single-precision bit patterns)
    for (const auto& [bits, cid] : float_const_ids)
        bin.writeInstruction(spv::OpConstant,
                              type_ids.at("float"),
                              cid,
                              bits);

    // ----- Global variable declarations (interface variables) -----
    for (const auto& [fname, epv] : ep_vars) {
        for (const auto& iv : epv.inputs)
            bin.writeInstruction(spv::OpVariable,
                                  iv.ptr_type_id,
                                  iv.var_id,
                                  static_cast<uint32_t>(iv.storage));
        if (epv.output.has_value())
            bin.writeInstruction(spv::OpVariable,
                                  epv.output->ptr_type_id,
                                  epv.output->var_id,
                                  static_cast<uint32_t>(epv.output->storage));
    }

    // ----- Function definitions (generated in Pass 2) -----
    for (uint32_t word : func_bin.get())
        bin.writeWord(word);

    return bin.get();
}

} // namespace neko
