if(NEKO_INSTALL)
	include(GNUInstallDirs)
	install(DIRECTORY modules/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/neko/modules
			FILES_MATCHING PATTERN "*.neko")
endif()

if(NEKO_GEN_STL)
	find_package (Python3 REQUIRED)

	add_custom_target(neko_gen_stl ALL
		COMMAND ${Python3_EXECUTABLE} gen.py
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	)
endif()
