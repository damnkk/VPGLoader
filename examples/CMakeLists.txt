find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

find_program(VPGLOADER_GLSLC
    NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
    REQUIRED
)

set(VPGLOADER_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(VPGLOADER_SHADER_OUTPUTS)

foreach(shader model.vert model.frag)
    set(shader_source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(shader_output "${VPGLOADER_SHADER_OUTPUT_DIR}/${shader}.spv")
    add_custom_command(
        OUTPUT "${shader_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VPGLOADER_SHADER_OUTPUT_DIR}"
        COMMAND "${VPGLOADER_GLSLC}" "${shader_source}" -o "${shader_output}"
        DEPENDS "${shader_source}"
        VERBATIM
    )
    list(APPEND VPGLOADER_SHADER_OUTPUTS "${shader_output}")
endforeach()

add_custom_target(vpgloader-vulkan-shaders DEPENDS ${VPGLOADER_SHADER_OUTPUTS})

add_executable(vpgloader-vulkan-viewer
    main.cpp
    VulkanViewer.cpp
    VulkanViewer.hpp
)
add_dependencies(vpgloader-vulkan-viewer vpgloader-vulkan-shaders)

target_compile_features(vpgloader-vulkan-viewer PRIVATE cxx_std_17)
target_compile_definitions(vpgloader-vulkan-viewer PRIVATE
    VPGLOADER_VIEWER_SHADER_DIR="${VPGLOADER_SHADER_OUTPUT_DIR}"
)
target_link_libraries(vpgloader-vulkan-viewer PRIVATE
    VPGLoader::VPGLoader
    Vulkan::Vulkan
    glfw
)

if(MSVC)
    target_compile_options(vpgloader-vulkan-viewer PRIVATE /utf-8)
endif()
