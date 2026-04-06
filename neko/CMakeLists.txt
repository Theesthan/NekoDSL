add_library(neko STATIC
	src/ast.hpp
	src/binary.cpp
	src/binary.tpp
	src/code_generator.cpp
	src/code_generator.tpp
	src/compiler.cpp
	src/neko_p.hpp
	src/neko_parser.hpp
	src/neko_parser.cpp
	src/parser.cpp
	src/parser.yy

	include/neko/neko.hpp
)

target_include_directories(neko PUBLIC include)

target_link_libraries(neko SPIRV-Tools SPIRV-Tools-opt SPIRV-Tools-link)

set_target_properties(neko PROPERTIES PUBLIC_HEADER include/neko/neko.hpp)
set_target_properties(neko PROPERTIES FOLDER "neko")

if(NEKO_INSTALL)
	include(GNUInstallDirs)
	install(TARGETS neko
			PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
	)
endif()

if(NEKO_GRAMMAR)
	add_custom_target(neko_grammar ALL
		COMMAND bison parser.yy -o parser.cpp.re
		COMMAND re2c parser.cpp.re -o parser.cpp
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src
	)
	set_target_properties(neko_grammar PROPERTIES FOLDER "neko" )
	add_dependencies(neko neko_grammar)
endif()
