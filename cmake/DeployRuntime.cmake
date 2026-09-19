# Keep build-tree executables runnable without a developer shell or custom PATH.
function(vpgloader_deploy_runtime target)
    if(NOT WIN32)
        return()
    endif()

    if(MSVC)
        get_filename_component(linker_directory "${CMAKE_LINKER}" DIRECTORY)
        find_program(VPGLOADER_DUMPBIN dumpbin HINTS "${linker_directory}" REQUIRED)
        set(dependency_tool dumpbin)
        set(dependency_command "${VPGLOADER_DUMPBIN}")
    else()
        set(dependency_tool objdump)
        set(dependency_command "${CMAKE_OBJDUMP}")
    endif()

    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DeployRuntimeWindows.cmake")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DEXECUTABLE=$<TARGET_FILE:${target}>"
            "-DRUNTIME_DLLS=$<TARGET_RUNTIME_DLLS:${target}>"
            "-DCMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM=windows+pe"
            "-DCMAKE_GET_RUNTIME_DEPENDENCIES_TOOL=${dependency_tool}"
            "-DCMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND=${dependency_command}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DeployRuntimeWindows.cmake"
        VERBATIM
        COMMENT "Deploying runtime DLLs for ${target}"
    )
endfunction()
