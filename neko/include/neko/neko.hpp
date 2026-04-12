#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace neko
{
	struct Options
	{
		bool debugInfo = false;
		bool validate = true;
		bool showDisassembly = false;
		bool profile = false;
		uint8_t optimizationLevel = 0;
		std::vector<std::string> moduleDirs; // search paths for import("...") resolution
	};

	class Compiler
	{
	public:
		std::vector<uint32_t> compile(const std::string& source) const;
		const Options& getOptions() const;
		void setOptions(const Options& newOptions);
	private:
		Options options;
	};
} // namespace neko
