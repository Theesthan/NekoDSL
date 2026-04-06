#include "neko_p.hpp"

#include <functional>
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
// Entry-point functions (with a decorator) must return void in SPIR-V;
// their NekoDSL return type is the output interface, not a function return.
const std::string& spirv_ret(const ast::FunctionDecl& func)
{
    static const std::string void_str;          // empty == void
    if (!func.decorator.empty()) return void_str;
    return func.return_type;
}

// Build a string key for a function type: "ret:param0:param1:…"
// An empty ret or "void" both produce the same key ("void").
std::string func_type_key(const std::string& ret,
                           const std::vector<ast::Param>& params)
{
    std::string key = (ret.empty() ? "void" : ret);
    for (const auto& p : params) { key += ':'; key += p.type; }
    return key;
}

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
    // Allocate ALL structured IDs before writing any instructions so that the
    // SPIR-V header Bound can be written correctly.
    // =========================================================================

    // --- Primitive types ---
    std::unordered_map<std::string, uint32_t> type_ids;
    type_ids["void"] = getNextId(); // OpTypeVoid

    // Collect non-void primitive types from function signatures
    for (const auto& func : ast.functions) {
        if (!func.return_type.empty() && !type_ids.count(func.return_type))
            type_ids[func.return_type] = getNextId();
        for (const auto& p : func.params)
            if (!type_ids.count(p.type))
                type_ids[p.type] = getNextId();
    }
    // Ensure int is registered when integer literals are present
    const auto int_literal_vals = all_int_literals(ast);
    if (!int_literal_vals.empty() && !type_ids.count("int"))
        type_ids["int"] = getNextId();

    const uint32_t void_id = type_ids.at("void");

    // Helper: look up a type ID by name ("" and "void" both map to void_id).
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
        const std::string key = func_type_key(spirv_ret(func), func.params);
        if (!ftype_ids.count(key))
            ftype_ids[key] = getNextId();
    }

    // Helper: look up the function-type ID for a given function.
    auto get_ftype_id = [&](const ast::FunctionDecl& func) -> uint32_t {
        return ftype_ids.at(func_type_key(spirv_ret(func), func.params));
    };

    // --- Function result IDs ---
    std::unordered_map<std::string, uint32_t> func_ids;
    for (const auto& func : ast.functions)
        func_ids[func.name] = getNextId();

    // --- Integer constant IDs ---
    std::unordered_map<int64_t, uint32_t> int_const_ids;
    for (int64_t v : int_literal_vals)
        int_const_ids[v] = getNextId();

    // --- Parameter IDs per function ---
    std::unordered_map<std::string, std::vector<uint32_t>> param_ids;
    for (const auto& func : ast.functions) {
        auto& pids = param_ids[func.name];
        for (size_t i = 0; i < func.params.size(); ++i)
            pids.push_back(getNextId());
    }

    // =========================================================================
    // Pass 2 — function body emission
    // Generate OpFunction … OpFunctionEnd blocks into a scratch Binary.
    // Label IDs and call-result IDs are allocated here (after Bound for Pass 3,
    // but that's fine — we patch the header after all IDs are allocated).
    // =========================================================================

    Binary func_bin;

    // Resolve expression → SPIR-V result ID, emitting instructions into func_bin.
    // 'locals' maps NekoDSL identifier names to their SPIR-V IDs.
    std::function<uint32_t(const ast::Expr&,
                            const std::unordered_map<std::string, uint32_t>&)>
    emit_expr = [&](const ast::Expr& e,
                    const std::unordered_map<std::string, uint32_t>& locals)
        -> uint32_t
    {
        switch (e.kind) {

        case ast::ExprKind::IntLiteral:
            return int_const_ids.at(e.int_val);

        case ast::ExprKind::Identifier: {
            auto it = locals.find(e.name);
            if (it == locals.end())
                throw std::runtime_error(
                    "NekoDSL: undefined identifier '" + e.name + "'");
            return it->second;
        }

        case ast::ExprKind::Call: {
            // Evaluate arguments first (left-to-right)
            std::vector<uint32_t> arg_result_ids;
            for (const auto& arg : e.args)
                arg_result_ids.push_back(emit_expr(arg, locals));

            auto fit = func_ids.find(e.name);
            if (fit == func_ids.end())
                throw std::runtime_error(
                    "NekoDSL: call to unknown function '" + e.name + "'");

            // Determine callee return type for OpFunctionCall Result Type field.
            uint32_t ret_type_id = void_id;
            for (const auto& f : ast.functions) {
                if (f.name == e.name) {
                    // Use the NekoDSL return type of the CALLEE (not the SPIR-V
                    // entry-point override) — callers are always regular functions.
                    ret_type_id = get_type_id(f.return_type);
                    break;
                }
            }

            const uint32_t result_id = getNextId();

            // Build operand list: [ResultType, Result, Function, Arg0, Arg1, …]
            std::vector<std::variant<uint32_t, const char*>> ops;
            ops.push_back(ret_type_id);
            ops.push_back(result_id);
            ops.push_back(fit->second); // function ID
            for (uint32_t a : arg_result_ids) ops.push_back(a);

            func_bin.writeInstruction(spv::OpFunctionCall, ops);
            return result_id;
        }
        } // switch
        throw std::runtime_error("NekoDSL: unknown expression kind");
    };

    // Emit each function in dependency order.
    for (const ast::FunctionDecl* func : topo_order(ast)) {
        // Build local symbol table from parameters
        std::unordered_map<std::string, uint32_t> locals;
        const auto& pids = param_ids.at(func->name);
        for (size_t i = 0; i < func->params.size(); ++i)
            locals[func->params[i].name] = pids[i];

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

        // OpFunctionParameter for each param
        for (size_t i = 0; i < func->params.size(); ++i)
            func_bin.writeInstruction(spv::OpFunctionParameter,
                                       get_type_id(func->params[i].type),
                                       pids[i]);

        // Entry block
        const uint32_t entry_label = getNextId();
        func_bin.writeInstruction(spv::OpLabel, entry_label);

        // Statements
        bool explicitly_returned = false;
        for (const auto& stmt : func->body.stmts) {
            if (stmt.kind == ast::StmtKind::ExprStmt) {
                emit_expr(*stmt.expr, locals); // result discarded
            } else { // Return
                if (spv_ret_type.empty()) {
                    // SPIR-V void return — evaluate expression for side effects only
                    if (stmt.expr.has_value())
                        emit_expr(*stmt.expr, locals);
                    func_bin.writeInstruction(spv::OpReturn);
                } else {
                    const uint32_t val = emit_expr(*stmt.expr, locals);
                    func_bin.writeInstruction(spv::OpReturnValue, val);
                }
                explicitly_returned = true;
            }
        }

        // Implicit return for functions that fall off the end
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
    bin.writeWords(spv::MagicNumber,    // Magic number
                   spv::Version,        // SPIR-V version
                   neko::MagicNumber,   // Generator magic (NekoDSL = 23)
                   final_bound,         // Bound (max ID + 1)
                   0u);                 // Reserved schema word

    // ----- OpCapability -----
    bin.writeInstruction(spv::OpCapability,
                          static_cast<uint32_t>(spv::CapabilityShader));

    // ----- OpExtInstImport (from extInsts registered before compile) -----
    for (const auto& [name, id] : extInsts)
        bin.writeInstruction(spv::OpExtInstImport, id, name.c_str());

    // ----- OpMemoryModel -----
    bin.writeInstruction(spv::OpMemoryModel,
                          static_cast<uint32_t>(spv::AddressingModelLogical),
                          static_cast<uint32_t>(spv::MemoryModelGLSL450));

    // ----- OpEntryPoint for every decorated function -----
    for (const auto& func : ast.functions) {
        if (func.decorator.empty()) continue;

        spv::ExecutionModel model = spv::ExecutionModelVertex;
        if      (func.decorator == "vertex")   model = spv::ExecutionModelVertex;
        else if (func.decorator == "fragment") model = spv::ExecutionModelFragment;

        // OpEntryPoint ExecutionModel FunctionId "Name" [Interface…]
        // We have no Interface variables for this simple implementation.
        bin.writeInstruction(spv::OpEntryPoint,
                              static_cast<uint32_t>(model),
                              func_ids.at(func.name),
                              func.name.c_str());
    }

    // ----- OpExecutionMode (required for Fragment entry points) -----
    for (const auto& func : ast.functions) {
        if (func.decorator == "fragment") {
            // Fragment shaders must declare an origin execution mode.
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

    // ----- Annotation (OpDecorate) -----
    for (const auto& [target, deco] : decorations)
        bin.writeInstruction(spv::OpDecorate,
                              target,
                              deco.first,
                              deco.second);

    // ----- Type declarations -----
    // OpTypeVoid must come first since it is referenced by function types.
    bin.writeInstruction(spv::OpTypeVoid, void_id);

    for (const auto& [name, tid] : type_ids) {
        if (name == "void") continue; // already emitted
        if      (name == "int")   bin.writeInstruction(spv::OpTypeInt,   tid, 32u, 1u);
        else if (name == "float") bin.writeInstruction(spv::OpTypeFloat, tid, 32u);
        else if (name == "bool")  bin.writeInstruction(spv::OpTypeBool,  tid);
        else
            throw std::runtime_error("NekoDSL: unsupported primitive type '" + name + "'");
    }

    // ----- Function-type declarations -----
    for (const auto& [key, tid] : ftype_ids) {
        // Parse "ret:p0:p1:…"
        std::vector<std::string> parts;
        std::string cur;
        for (char c : key) {
            if (c == ':') { parts.push_back(cur); cur.clear(); }
            else          { cur += c; }
        }
        parts.push_back(cur);

        const uint32_t ret_id =
            (parts[0] == "void") ? void_id : type_ids.at(parts[0]);

        // Operand list: [ResultId, ReturnTypeId, ParamTypeId0, …]
        std::vector<std::variant<uint32_t, const char*>> ops;
        ops.push_back(tid);
        ops.push_back(ret_id);
        for (size_t i = 1; i < parts.size(); ++i)
            if (!parts[i].empty())
                ops.push_back(type_ids.at(parts[i]));

        bin.writeInstruction(spv::OpTypeFunction, ops);
    }

    // ----- Constant declarations -----
    for (const auto& [val, cid] : int_const_ids)
        bin.writeInstruction(spv::OpConstant,
                              type_ids.at("int"),
                              cid,
                              static_cast<uint32_t>(val));

    // ----- Function definitions (generated in Pass 2) -----
    for (uint32_t word : func_bin.get())
        bin.writeWord(word);

    return bin.get();
}

} // namespace neko
