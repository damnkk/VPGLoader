cmake_minimum_required(VERSION 3.24)

project(VPGLoader VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(BUILD_SHARED_LIBS "Build VPGLoader as a shared library" OFF)
option(VPGLOADER_BUILD_EXAMPLES "Build VPGLoader example programs" OFF)
option(VPGLOADER_ENABLE_PACKAGING "Generate CPack package metadata" ON)

find_package(assimp CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)

add_library(VPGLoader
    src/VPGLoader.cpp
    src/image/ImageLoader.cpp
    src/model/ModelLoader.cpp
    src/texture/Texture.cpp
    src/texture/TextureCache.cpp
    src/texture/TextureConverter.cpp
    src/texture/TextureLoader.cpp
)
add_library(VPGLoader::VPGLoader ALIAS VPGLoader)

target_compile_features(VPGLoader PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(VPGLoader PRIVATE /utf-8)
endif()
target_include_directories(VPGLoader
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(VPGLoader
    PUBLIC
        assimp::assimp
        KTX::ktx
        OpenImageIO::OpenImageIO
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(VPGLoader PRIVATE VPGLOADER_BUILDING_LIBRARY)
else()
    target_compile_definitions(VPGLoader PUBLIC VPGLOADER_STATIC)
endif()

set_target_properties(VPGLoader PROPERTIES
    EXPORT_NAME VPGLoader
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(VPGLOADER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS VPGLoader
    EXPORT VPGLoaderTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/VPGLoaderConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/VPGLoaderConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VPGLoader
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/VPGLoaderConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT VPGLoaderTargets
    FILE VPGLoaderTargets.cmake
    NAMESPACE VPGLoader::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VPGLoader
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/VPGLoaderConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/VPGLoaderConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VPGLoader
)

if(VPGLOADER_ENABLE_PACKAGING)
    set(CPACK_PACKAGE_NAME "VPGLoader")
    set(CPACK_PACKAGE_VENDOR "VPGLoader")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()
