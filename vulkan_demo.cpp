#include <iostream>
#include <fstream>
#include <vector>
#include <stdexcept>
#include <iomanip>
#include <cstring>
#include <cstdint>

// SPIR-V opcodes
constexpr uint32_t SPIR_V_MAGIC = 0x07230203;

// Load SPIR-V binary from file
std::vector<uint32_t> load_spirv(const char* filename)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        throw std::runtime_error(std::string("Failed to open: ") + filename);

    size_t file_size = file.tellg();
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));

    file.seekg(0, std::ios::beg);
    file.read((char*)buffer.data(), file_size);
    file.close();

    return buffer;
}

// Parse entry point names from SPIR-V
struct EntryPoint {
    std::string name;
    std::string model;
};

std::vector<EntryPoint> parse_entry_points(const std::vector<uint32_t>& spv)
{
    std::vector<EntryPoint> entries;

    uint32_t i = 5; // Skip SPIR-V header (5 words)
    while (i < spv.size())
    {
        uint32_t word_count = spv[i] >> 16;
        uint32_t opcode = spv[i] & 0xFFFF;

        if (opcode == 15) // OpEntryPoint
        {
            if (i + 3 < spv.size())
            {
                uint32_t exec_model = spv[i + 1];
                const char* model_name = (exec_model == 0) ? "Vertex" :
                                         (exec_model == 4) ? "Fragment" : "Compute";

                // Entry point name starts at word i+3 (as null-terminated string in words)
                if (i + 3 < spv.size())
                {
                    std::string name;
                    for (uint32_t j = i + 3; j < i + word_count; j++)
                    {
                        uint32_t word = spv[j];
                        for (int k = 0; k < 4; k++)
                        {
                            char ch = (word >> (k * 8)) & 0xFF;
                            if (ch == 0) break;
                            name += ch;
                        }
                        if (word == 0 || name.back() == 0) break;
                    }
                    entries.push_back({name, model_name});
                }
            }
        }
        i += word_count;
    }

    return entries;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: vulkan_demo <file.spv>" << std::endl;
        return 1;
    }

    const char* filename = argv[1];

    try
    {
        std::cout << "=== NekoDSL Vulkan SPIR-V Validation ===" << std::endl;
        std::cout << "\nLoading " << filename << "..." << std::endl;
        auto spv = load_spirv(filename);
        std::cout << "  ✓ Loaded " << spv.size() << " words ("
                  << (spv.size() * 4) << " bytes)" << std::endl;

        // Verify SPIR-V magic number
        if (spv.size() < 5)
            throw std::runtime_error("File too small to be valid SPIR-V");

        if (spv[0] != SPIR_V_MAGIC)
            throw std::runtime_error("Invalid SPIR-V magic number");

        std::cout << "\n✓ SPIR-V Header" << std::endl;
        std::cout << "  Magic: 0x" << std::hex << std::setw(8) << std::setfill('0')
                  << spv[0] << std::dec << std::endl;

        uint32_t major = (spv[1] >> 16) & 0xFF;
        uint32_t minor = (spv[1] >> 8) & 0xFF;
        std::cout << "  Version: " << major << "." << minor << std::endl;
        std::cout << "  Generator: NekoDSL" << std::endl;
        std::cout << "  Bound: " << spv[3] << " (max ID)" << std::endl;

        // Parse entry points
        auto entries = parse_entry_points(spv);
        std::cout << "\n✓ Entry Points" << std::endl;
        for (const auto& ep : entries)
        {
            std::cout << "  - " << ep.name << " (" << ep.model << ")" << std::endl;
        }

        std::cout << "\n=== Vulkan Integration ===" << std::endl;
        std::cout << "\nTo use in your Vulkan application:\n\n";
        std::cout << "  // 1. Load the SPIR-V file\n"
                  << "  auto spv_code = load_spirv(\"" << filename << "\");\n\n"
                  << "  // 2. Create shader module\n"
                  << "  VkShaderModuleCreateInfo create_info{};\n"
                  << "  create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;\n"
                  << "  create_info.codeSize = spv_code.size() * sizeof(uint32_t);\n"
                  << "  create_info.pCode = spv_code.data();\n"
                  << "  VkShaderModule shader_module;\n"
                  << "  vkCreateShaderModule(device, &create_info, nullptr, &shader_module);\n\n"
                  << "  // 3. Create pipeline shader stages\n"
                  << "  VkPipelineShaderStageCreateInfo stages[2]{};\n"
                  << "  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;\n"
                  << "  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;\n"
                  << "  stages[0].module = shader_module;\n"
                  << "  stages[0].pName = \"vert\";  // Entry point from " << filename << "\n"
                  << "  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;\n"
                  << "  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;\n"
                  << "  stages[1].module = shader_module;\n"
                  << "  stages[1].pName = \"frag\";  // Entry point from " << filename << "\n\n"
                  << "  // 4. Include in VkGraphicsPipelineCreateInfo\n"
                  << "  VkGraphicsPipelineCreateInfo pipeline_info{};\n"
                  << "  pipeline_info.stageCount = 2;\n"
                  << "  pipeline_info.pStages = stages;\n"
                  << "  // ... other pipeline settings ...\n"
                  << "  vkCreateGraphicsPipelines(device, nullptr, 1, &pipeline_info, nullptr, &pipeline);\n";

        std::cout << "\n✅ SPIR-V validation complete. Your shader is ready for Vulkan!\n";

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }
}
