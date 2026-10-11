# Compile upstream entry points under private names, then export the guest
# descriptor hooks. Internal calls remain native and never translate twice.
set(descriptorHookManifest "${CMAKE_CURRENT_LIST_DIR}/include/FileDescriptorHooks.def")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${descriptorHookManifest}")
file(STRINGS "${descriptorHookManifest}" descriptorHookRows REGEX "^FD_")
set(descriptorHookDefinitions)
set(descriptorHookSymbols)
foreach(row IN LISTS descriptorHookRows)
    if(row MATCHES "^FD_CUSTOM\\(([A-Za-z0-9_]+)\\)")
        set(symbol "${CMAKE_MATCH_1}")
    elseif(row MATCHES "^FD_(KERNEL|POSIX)\\([^,]+, ([A-Za-z0-9_]+),")
        set(symbol "${CMAKE_MATCH_2}")
    else()
        message(FATAL_ERROR "Invalid descriptor hook: ${row}")
    endif()
    list(APPEND descriptorHookSymbols "${symbol}")
    list(APPEND descriptorHookDefinitions "${symbol}=${symbol}_native_nid_no_patch")
endforeach()
# Refuse an upstream descriptor entry point that bypasses isolation.
foreach(source IN ITEMS Open Stdio Terminal SharedMemory)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/src/${source}.cpp")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/src/${source}.cpp" sourceText)
    string(REGEX MATCHALL "APS5_VABI[ \t]+[A-Za-z0-9_]+\\((int|std::int32_t)[ \t]+(d|fd|descriptor|from)[,)][^;{}]*\\{" descriptorEntries "${sourceText}")
    foreach(entry IN LISTS descriptorEntries)
        string(REGEX REPLACE "APS5_VABI[ \t]+([A-Za-z0-9_]+)\\(.*" "\\1" symbol "${entry}")
        if(NOT symbol IN_LIST descriptorHookSymbols)
            message(FATAL_ERROR "Unhooked guest descriptor entry point: ${source}.cpp:${symbol}")
        endif()
    endforeach()
endforeach()
set_property(SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/src/Open.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Stdio.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Terminal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/SharedMemory.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS ${descriptorHookDefinitions})
target_sources(libkernel PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/FileDescriptorHooks.cpp")

# Directory helpers receive native descriptors from the upstream bodies.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/src/DirectoryDescriptor.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "DescriptorIsOpen_nid_no_patch=DescriptorIsOpen_nid_no_patch_native_nid_no_patch"
    "GetDirectoryDescriptorPath_nid_no_patch=GetDirectoryDescriptorPath_native_nid_no_patch")

# select needs leases for all selected files, in addition to virtual sockets.
set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/../Socket/src/Socket.cpp" APPEND PROPERTY COMPILE_DEFINITIONS
    "select_nid_postfix=select_nid_postfix_native_nid_no_patch"
    "KernelSetSocketPoller_nid_no_patch=KernelSetSocketPoller_nid_no_patch_native_nid_no_patch")
target_sources(libkernel PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/GuestSelect.cpp")

if(BUILD_TESTING)
    foreach(pair IN ITEMS
            "guest_file_isolation;GuestFileIsolation"
            "guest_file_seek;GuestFileSeek")
        list(GET pair 0 testName)
        list(GET pair 1 sourceName)
        add_test_executable(${testName}_tests ${LIBS_INCLUDE_DIR}/tests/${sourceName}.cpp)
        target_include_directories(${testName}_tests PRIVATE ${LIBS_INCLUDE_DIR})
        target_link_libraries(${testName}_tests PRIVATE libkernel libc)
        configure_windows_unwind(${testName}_tests)
        add_test(NAME ${testName} COMMAND ${testName}_tests)
        set_tests_properties(${testName} PROPERTIES TIMEOUT 20)
        if(WIN32)
            set_tests_properties(${testName} PROPERTIES ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:libkernel>")
        endif()
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        foreach(sourceName IN ITEMS GuestFileHostIsolation GuestFileDescriptorLease GuestFileDescriptorOwnership GuestStdioErrno GuestSelect)
            add_test_executable(${sourceName}_tests ${LIBS_INCLUDE_DIR}/tests/${sourceName}.cpp)
            target_include_directories(${sourceName}_tests PRIVATE ${LIBS_INCLUDE_DIR})
            target_link_libraries(${sourceName}_tests PRIVATE libkernel libc)
        endforeach()
        foreach(mode kernel posix underscore stdio fdopen stdio-stale fdopen-stale standard)
            string(REPLACE "-" "_" suffix "${mode}")
            add_test(NAME guest_file_host_isolation_${suffix} COMMAND GuestFileHostIsolation_tests ${mode})
            set_tests_properties(guest_file_host_isolation_${suffix} PROPERTIES TIMEOUT 20)
        endforeach()
        foreach(mode lease race blocked-read pipe-rollback)
            string(REPLACE "-" "_" suffix "${mode}")
            add_test(NAME guest_file_descriptor_${suffix} COMMAND GuestFileDescriptorLease_tests ${mode})
            set_tests_properties(guest_file_descriptor_${suffix} PROPERTIES TIMEOUT 30)
        endforeach()
        add_test(NAME guest_file_descriptor_ownership COMMAND GuestFileDescriptorOwnership_tests)
        add_test(NAME guest_stdio_errno COMMAND GuestStdioErrno_tests)
        add_test(NAME guest_select COMMAND GuestSelect_tests)
    endif()
endif()
