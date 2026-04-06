#include "neko_p.hpp"
#include "neko_parser.hpp"

#include <iostream>
#include <stdexcept>

namespace neko
{
	std::vector<uint32_t> Compiler::compile(const std::string& source) const
	{
		// Parse NekoDSL source into an AST
		const ast::TranslationUnit ast = parse_source(source);

		// Lower AST to SPIR-V binary
		CodeGenerator cg;
		std::vector<uint32_t> binary = cg.generate(ast, options);

		if (options.validate || options.showDisassembly)
		{
			spvtools::SpirvTools core(neko::SpvTargetEnv);

			auto print_msg_to_stderr = [](spv_message_level_t, const char*,
				const spv_position_t&, const char* m) {
					std::cerr << "error: " << m << std::endl;
			};

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
