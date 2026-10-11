add_library(libkernel_sys SHARED EXCLUDE_FROM_ALL Export.cpp DescriptorHooks.cpp)
target_include_directories(libkernel_sys PRIVATE ${LIBS_INCLUDE_DIR})
set_property(SOURCE Export.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "fchdir_nid_postfix=fchdir_nid_postfix_native_nid_no_patch"
    "GetDirectoryDescriptorPath_nid_no_patch=GetDirectoryDescriptorPath_native_nid_no_patch")
configure_windows_unwind(libkernel_sys)
