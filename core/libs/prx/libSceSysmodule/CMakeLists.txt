add_library(libSceSysmodule SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceSysmodule PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceSysmodule)

if(BUILD_TESTING)
    add_test_executable(guest_sysmodule_tests ${LIBS_INCLUDE_DIR}/tests/GuestSysmodule.cpp)
    target_include_directories(guest_sysmodule_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_sysmodule_tests PRIVATE libSceSysmodule libkernel libc)
    configure_windows_unwind(guest_sysmodule_tests)
    set(guestSysmoduleRun ${CMAKE_CURRENT_BINARY_DIR}/guest_sysmodule)
    file(MAKE_DIRECTORY ${guestSysmoduleRun}/libs)
    add_custom_target(guest_sysmodule_runtime
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:libSceFiber>" "${guestSysmoduleRun}/libs/libSceFiber.prx"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:libSceAudioPropagation>" "${guestSysmoduleRun}/libs/libSceAudioPropagation.prx"
            VERBATIM
    )
    add_dependencies(guest_sysmodule_runtime libSceFiber libSceAudioPropagation)
    add_dependencies(guest_sysmodule_tests guest_sysmodule_runtime)
    add_test(NAME guest_sysmodule COMMAND guest_sysmodule_tests)
    set_tests_properties(guest_sysmodule PROPERTIES WORKING_DIRECTORY ${guestSysmoduleRun})
    if(UNIX AND NOT APPLE)
        add_test(NAME guest_sysmodule_maps_scan COMMAND guest_sysmodule_tests)
        set_tests_properties(guest_sysmodule_maps_scan PROPERTIES ENVIRONMENT "APS5_NO_PROCMAP_QUERY=1"
            WORKING_DIRECTORY ${guestSysmoduleRun})
    endif()
    if(WIN32)
        set_tests_properties(guest_sysmodule PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSysmodule>")
    endif()

    add_test_executable(guest_sysmodule_initial_cleanup_tests ${LIBS_INCLUDE_DIR}/tests/SysmoduleInitialCleanup.cpp)
    target_include_directories(guest_sysmodule_initial_cleanup_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_sysmodule_initial_cleanup_tests PRIVATE libSceSysmodule libc)
    configure_windows_unwind(guest_sysmodule_initial_cleanup_tests)
    add_test(NAME guest_sysmodule_initial_cleanup_strict COMMAND guest_sysmodule_initial_cleanup_tests --strict)
    set_tests_properties(guest_sysmodule_initial_cleanup_strict PROPERTIES ENVIRONMENT_MODIFICATION
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=unset:")
    add_test(NAME guest_sysmodule_initial_cleanup_once COMMAND guest_sysmodule_initial_cleanup_tests --initial-cleanup)
    add_test(NAME guest_sysmodule_initial_cleanup_after_load COMMAND guest_sysmodule_initial_cleanup_tests --load-first)
    add_test(NAME guest_sysmodule_initial_cleanup_after_internal_load COMMAND guest_sysmodule_initial_cleanup_tests --internal-load-first)
    add_test(NAME guest_sysmodule_initial_cleanup_after_internal_arg_load COMMAND guest_sysmodule_initial_cleanup_tests --internal-arg-load-first)
    set_tests_properties(guest_sysmodule_initial_cleanup_once
        guest_sysmodule_initial_cleanup_after_load
        guest_sysmodule_initial_cleanup_after_internal_load
        guest_sysmodule_initial_cleanup_after_internal_arg_load PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=1")
    add_test(NAME guest_sysmodule_initial_cleanup_disabled COMMAND guest_sysmodule_initial_cleanup_tests --strict)
    set_tests_properties(guest_sysmodule_initial_cleanup_disabled PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=0")
    add_test(NAME guest_sysmodule_initial_cleanup_invalid COMMAND guest_sysmodule_initial_cleanup_tests --strict)
    set_tests_properties(guest_sysmodule_initial_cleanup_invalid PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=true")
    add_test(NAME guest_sysmodule_initial_cleanup_padded COMMAND guest_sysmodule_initial_cleanup_tests --strict)
    set_tests_properties(guest_sysmodule_initial_cleanup_padded PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=01")
    set(sysmodule_initial_cleanup_tests
        guest_sysmodule_initial_cleanup_strict
        guest_sysmodule_initial_cleanup_once
        guest_sysmodule_initial_cleanup_after_load
        guest_sysmodule_initial_cleanup_after_internal_load
        guest_sysmodule_initial_cleanup_after_internal_arg_load
        guest_sysmodule_initial_cleanup_disabled
        guest_sysmodule_initial_cleanup_invalid
        guest_sysmodule_initial_cleanup_padded)
    set_tests_properties(${sysmodule_initial_cleanup_tests} PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_property(TEST ${sysmodule_initial_cleanup_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSysmodule>")
    endif()
endif()
