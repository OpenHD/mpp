# vim: syntax=cmake
# ----------------------------------------------------------------------------
# mpp built-in unit test case
# ----------------------------------------------------------------------------
# macro for adding mpp sub-module unit test
find_package(PkgConfig)
pkg_check_modules(DRM QUIET libdrm)

macro(add_mpp_test module ext)
    set(test_name ${module}_test)
    set(file_name ${test_name}.${ext})
    string(TOUPPER ${test_name} test_tag)
    #message(STATUS "moduule   : ${module}")
    #message(STATUS "test_name : ${test_name}")
    #message(STATUS "test_tag  : ${test_tag}")

    option(${test_tag} "Build mpp ${module}.${ext} unit test" ${BUILD_TEST})
    if(${test_tag})
        add_executable(${test_name} ${file_name} mpp_event_trigger.c mpp_parse_cfg.c)
        if(DRM_FOUND)
            target_include_directories(${test_name} PRIVATE ${DRM_INCLUDE_DIRS})
            target_link_libraries(${test_name} ${MPP_SHARED} utils ${DRM_LIBRARIES})
        else()
            target_link_libraries(${test_name} ${MPP_SHARED} utils)
        endif()
        set_target_properties(${test_name} PROPERTIES FOLDER "test")
        install(TARGETS ${test_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        #add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endmacro()

if(DRM_FOUND)
add_mpp_test(openhd_vid c)
endif()


# mpp info test
add_mpp_test(mpp_info c)

# mpi decoder unit test
add_mpp_test(mpi_dec c)

# mpi decoder multi-thread input / output unit test
add_mpp_test(mpi_dec_mt c)

# mpi decoder no-thread input / output unit test
add_mpp_test(mpi_dec_nt c)

# mpi encoder unit test
add_mpp_test(mpi_enc c)

# mpi encoder multi-thread input / output unit test
add_mpp_test(mpi_enc_mt c)

# new mpi rc unit test
add_mpp_test(mpi_rc2 c)

# new dec multi unit test
add_mpp_test(mpi_dec_multi c)

# simple v4l2 to udp encoder test
add_mpp_test(mpi_v4l2_udp cpp)

macro(add_legacy_test module)
    set(test_name ${module}_test)
    string(TOUPPER ${test_name} test_tag)
    #message(STATUS "moduule   : ${module}")
    #message(STATUS "test_name : ${test_name}")
    #message(STATUS "test_tag  : ${test_tag}")

    option(${test_tag} "Build legacy ${module} unit test" ${BUILD_TEST})
    if(${test_tag})
        add_executable(${test_name} ${test_name}.c)
        if(ASAN_CHECK)
            target_link_libraries(${test_name} ${ASAN_LIB} pthread dl)
        else(ASAN_CHECK)
            target_link_libraries(${test_name} dl)
        endif(ASAN_CHECK)
        set_target_properties(${test_name} PROPERTIES FOLDER "test")
        install(TARGETS ${test_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endmacro()

# legacy vpu_api unit test
add_legacy_test(vpu_api)
