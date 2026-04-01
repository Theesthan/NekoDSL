add_executable(nekoc
	src/nekoc.cpp
)

target_link_libraries(nekoc neko argparse)

set_target_properties(nekoc PROPERTIES FOLDER "neko")

if(NEKO_INSTALL)
	include(GNUInstallDirs)
	install(TARGETS nekoc DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
