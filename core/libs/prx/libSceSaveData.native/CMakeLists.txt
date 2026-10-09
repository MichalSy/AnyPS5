add_library(libSceSaveData.native SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceSaveData.native PRIVATE ${LIBS_INCLUDE_DIR})
configure_windows_unwind(libSceSaveData.native)

if(BUILD_TESTING)
    add_test_executable(guest_savedata_search_tests ${LIBS_INCLUDE_DIR}/tests/GuestSaveDataSearch.cpp)
    target_include_directories(guest_savedata_search_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_savedata_search_tests PRIVATE libSceSaveData.native libc)
    configure_windows_unwind(guest_savedata_search_tests)
    add_test(NAME guest_savedata_search COMMAND guest_savedata_search_tests)
    if(WIN32)
        set_tests_properties(guest_savedata_search PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSaveData.native>")
    endif()

    add_test_executable(guest_savedata_delete_tests ${LIBS_INCLUDE_DIR}/tests/GuestSaveDataDelete.cpp)
    target_include_directories(guest_savedata_delete_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_savedata_delete_tests PRIVATE libSceSaveData.native libc)
    add_test(NAME guest_savedata_delete COMMAND guest_savedata_delete_tests)
    if(WIN32)
        set_tests_properties(guest_savedata_delete PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSaveData.native>")
    endif()

    add_test_executable(guest_savedata_ps4_tests ${LIBS_INCLUDE_DIR}/tests/GuestSaveDataPs4.cpp)
    target_include_directories(guest_savedata_ps4_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_savedata_ps4_tests PRIVATE libSceSaveData.native libc)
    configure_windows_unwind(guest_savedata_ps4_tests)
    add_test(NAME guest_savedata_ps4 COMMAND guest_savedata_ps4_tests)
    if(WIN32)
        set_tests_properties(guest_savedata_ps4 PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSaveData.native>")
    endif()

    add_test_executable(guest_savedata_lifecycle_tests ${LIBS_INCLUDE_DIR}/tests/SaveDataLifecycle.cpp)
    target_include_directories(guest_savedata_lifecycle_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_savedata_lifecycle_tests PRIVATE libSceSaveData.native libc)
    configure_windows_unwind(guest_savedata_lifecycle_tests)
    add_test(NAME guest_savedata_lifecycle_strict COMMAND guest_savedata_lifecycle_tests --strict)
    set_tests_properties(guest_savedata_lifecycle_strict PROPERTIES ENVIRONMENT_MODIFICATION
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=unset:")
    add_test(NAME guest_savedata_lifecycle_initial_cleanup COMMAND guest_savedata_lifecycle_tests --initial-cleanup)
    add_test(NAME guest_savedata_lifecycle_initial_cleanup_active COMMAND guest_savedata_lifecycle_tests --active-initial)
    add_test(NAME guest_savedata_lifecycle_initial_cleanup_after_init COMMAND guest_savedata_lifecycle_tests --init-first)
    set_tests_properties(guest_savedata_lifecycle_initial_cleanup
        guest_savedata_lifecycle_initial_cleanup_active
        guest_savedata_lifecycle_initial_cleanup_after_init PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=1")
    add_test(NAME guest_savedata_lifecycle_initial_cleanup_disabled COMMAND guest_savedata_lifecycle_tests --strict)
    set_tests_properties(guest_savedata_lifecycle_initial_cleanup_disabled PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=0")
    add_test(NAME guest_savedata_lifecycle_initial_cleanup_invalid COMMAND guest_savedata_lifecycle_tests --strict)
    set_tests_properties(guest_savedata_lifecycle_initial_cleanup_invalid PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=true")
    add_test(NAME guest_savedata_lifecycle_initial_cleanup_padded COMMAND guest_savedata_lifecycle_tests --strict)
    set_tests_properties(guest_savedata_lifecycle_initial_cleanup_padded PROPERTIES ENVIRONMENT
        "ANYPS5_SAVEDATA_INITIAL_CLEANUP=01")
    set(savedata_lifecycle_tests
        guest_savedata_lifecycle_strict
        guest_savedata_lifecycle_initial_cleanup
        guest_savedata_lifecycle_initial_cleanup_active
        guest_savedata_lifecycle_initial_cleanup_after_init
        guest_savedata_lifecycle_initial_cleanup_disabled
        guest_savedata_lifecycle_initial_cleanup_invalid
        guest_savedata_lifecycle_initial_cleanup_padded)
    set_tests_properties(${savedata_lifecycle_tests} PROPERTIES TIMEOUT 20)
    if(WIN32)
        set_property(TEST ${savedata_lifecycle_tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceSaveData.native>"
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libc>")
    endif()
endif()
