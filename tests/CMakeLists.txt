add_executable(vpgloader-model-tests ModelTests.cpp)
target_link_libraries(vpgloader-model-tests PRIVATE VPGLoader::VPGLoader)
vpgloader_deploy_runtime(vpgloader-model-tests)
add_test(NAME vpgloader-model-tests
    COMMAND vpgloader-model-tests "${CMAKE_CURRENT_BINARY_DIR}/assets")
