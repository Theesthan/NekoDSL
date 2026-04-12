#include "neko_p.hpp"
#include "neko_parser.hpp"

#include <spirv-tools/optimizer.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace {

// Join two path segments with a forward slash (works on Windows too).
std::string join_path(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    const char last = dir.back();
    return (last == '/' || last == '\\') ? dir + name : dir + '/' + name;
}

// Try to open a file and read its entire contents. Returns nullopt if absent.
std::optional<std::string> try_read_file(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::string s{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return std::nullopt;
    return s;
}

// Find a module by name in the given search dirs. Returns file content or nullopt.
std::optional<std::string> resolve_module(const std::string& name,
                                           const std::vector<std::string>& dirs)
{
    // Normalise: ensure .neko suffix for lookup
    const std::string name_neko =
        (name.size() > 5 && name.substr(name.size() - 5) == ".neko")
        ? name : name + ".neko";

    // Try the name as-is (absolute or relative to CWD)
    if (auto s = try_read_file(name_neko)) return s;
    if (auto s = try_read_file(name))      return s;

    // Try in each search directory
    for (const auto& dir : dirs) {
        if (auto s = try_read_file(join_path(dir, name_neko))) return s;
        if (auto s = try_read_file(join_path(dir, name)))      return s;
    }
    return std::nullopt;
}

// Merge src into dst, skipping functions/uniforms with names already present.
void merge_tu(neko::ast::TranslationUnit& dst, neko::ast::TranslationUnit src)
{
    std::unordered_set<std::string> existing_fns;
    for (const auto& f : dst.functions) existing_fns.insert(f.name);
    for (auto& f : src.functions)
        if (!existing_fns.count(f.name))
            dst.functions.push_back(std::move(f));

    std::unordered_set<std::string> existing_ubos;
    for (const auto& u : dst.uniforms) existing_ubos.insert(u.name);
    for (auto& u : src.uniforms)
        if (!existing_ubos.count(u.name))
            dst.uniforms.push_back(std::move(u));

    std::unordered_set<std::string> existing_smpls;
    for (const auto& s : dst.samplers) existing_smpls.insert(s.name);
    for (auto& s : src.samplers)
        if (!existing_smpls.count(s.name))
            dst.samplers.push_back(std::move(s));
}

// Recursively resolve imports, depth-first so callees precede callers.
neko::ast::TranslationUnit resolve_imports(const std::string& source,
                                            const std::vector<std::string>& dirs,
                                            std::unordered_set<std::string>& visited)
{
    neko::ast::TranslationUnit tu = neko::parse_source(source);

    neko::ast::TranslationUnit result;
    for (const auto& imp : tu.imports) {
        if (visited.count(imp)) continue;
        visited.insert(imp);
        auto content = resolve_module(imp, dirs);
        if (!content) continue; // silently skip missing modules
        merge_tu(result, resolve_imports(*content, dirs, visited));
    }
    merge_tu(result, std::move(tu));
    return result;
}

} // anonymous namespace

namespace neko
{
	std::vector<uint32_t> Compiler::compile(const std::string& source) const
	{
		// Parse NekoDSL source + resolve imports into a merged AST
		std::unordered_set<std::string> visited;
		const ast::TranslationUnit ast =
		    resolve_imports(source, options.moduleDirs, visited);

		// Lower AST to SPIR-V binary
		CodeGenerator cg;
		std::vector<uint32_t> binary = cg.generate(ast, options);

		auto print_msg_to_stderr = [](spv_message_level_t, const char*,
			const spv_position_t&, const char* m) {
				std::cerr << "error: " << m << std::endl;
		};

		// Optimization
		if (options.optimizationLevel > 0)
		{
			spvtools::Optimizer opt(neko::SpvTargetEnv);
			opt.SetMessageConsumer(print_msg_to_stderr);

			if (options.optimizationLevel >= 2)
				opt.RegisterPerformancePasses();
			else
				opt.RegisterSizePasses();

			std::vector<uint32_t> optimized;
			if (!opt.Run(binary.data(), binary.size(), &optimized))
				throw std::runtime_error("SPIR-V optimization failed");
			binary = std::move(optimized);
		}

		if (options.validate || options.showDisassembly)
		{
			spvtools::SpirvTools core(neko::SpvTargetEnv);
			core.SetMessageConsumer(print_msg_to_stderr);

			// Validation
			if (options.validate)
			{
				if (!core.Validate(binary))
					throw std::runtime_error("SPIR-V validation failed");
			}

			// Disassembly
			if (options.showDisassembly)
			{
				std::string disassembly;
				if (!core.Disassemble(binary, &disassembly))
					throw std::runtime_error("SPIR-V disassembly failed");
				std::cout << disassembly << "\n";
			}
		}

		return binary;
	}

	const Options& Compiler::getOptions() const
	{
		return options;
	}

	void Compiler::setOptions(const Options& newOptions)
	{
		if (newOptions.optimizationLevel < 0 || newOptions.optimizationLevel > 4)
		{
			throw std::runtime_error("Invalid optimization level");
		}
		options = newOptions;
	}
} // namespace neko
