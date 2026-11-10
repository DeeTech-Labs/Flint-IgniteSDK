cmake_minimum_required(VERSION 3.21)
project(FlintIgniteSDK VERSION 1.0 LANGUAGES C CXX)

add_library(flint_platform STATIC
    src/manifest.cpp
    src/storage.cpp
    src/resources.cpp
    src/lua_app.cpp
    src/runtime.cpp
    src/core.cpp
    src/fonts.cpp
)
add_library(Flint::Platform ALIAS flint_platform)
set_target_properties(flint_platform PROPERTIES EXPORT_NAME Platform)
target_include_directories(flint_platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(flint_platform PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(flint_platform PUBLIC /utf-8)
    target_compile_definitions(flint_platform PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(flint_platform PRIVATE -Wall -Wextra)
endif()

option(FLINT_ENABLE_LUA "Enable bounded experimental Lua execution on host" ON)
if(FLINT_ENABLE_LUA)
    include(cmake/LuaSources.cmake)
    target_sources(flint_platform PRIVATE ${FLINT_LUA_CORE})
    target_include_directories(flint_platform PRIVATE third_party/lua)
    target_compile_definitions(flint_platform PUBLIC ${FLINT_LUA_DEFINITIONS})
endif()

option(FLINT_SDK_TOOLS "Build host-side Ignite SDK validator" ON)
if(FLINT_SDK_TOOLS)
    add_executable(ignite-run tools/sdk_run.cpp)
    target_link_libraries(ignite-run PRIVATE flint_platform)
    add_executable(ignite-check tools/sdk_check.cpp)
    target_link_libraries(ignite-check PRIVATE flint_platform)
endif()

option(FLINT_PLATFORM_TESTS "Build local platform tests" ON)
if(FLINT_PLATFORM_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME companion_client COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_companion.py)
    add_test(NAME registry_validation COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registry.py)
    add_executable(resource_tests tests/resources.cpp)
    target_link_libraries(resource_tests PRIVATE flint_platform)
    add_test(NAME resource_rendering COMMAND resource_tests)
    add_executable(core_tests tests/core.cpp)
    target_link_libraries(core_tests PRIVATE flint_platform)
    add_test(NAME core_integration
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_core.py $<TARGET_FILE:core_tests>
    )
    add_executable(platform_tests tests/contracts.cpp)
    target_link_libraries(platform_tests PRIVATE flint_platform)
    add_test(NAME contracts COMMAND platform_tests)
    add_test(NAME package_interop
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_builder.py $<TARGET_FILE:platform_tests>
    )
endif()

if(FLINT_PLATFORM_TESTS AND FLINT_SDK_TOOLS)
    add_test(NAME sdk_workflow COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdk.py $<TARGET_FILE:ignite-check>)
endif()

# Standalone SDK installation and dependency exports.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS flint_platform EXPORT FlintPlatformTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT FlintPlatformTargets NAMESPACE Flint::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlintPlatform)
configure_package_config_file(cmake/FlintPlatformConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FlintPlatformConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlintPlatform)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/FlintPlatformConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/FlintPlatformConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FlintPlatformConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FlintPlatform)
if(FLINT_SDK_TOOLS)
    install(TARGETS ignite-check ignite-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(PROGRAMS tools/ignite.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME ignite)
    install(FILES tools/compile_ignite.py tools/build_package.py
        DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(FLINT_PLATFORM_TESTS AND FLINT_SDK_TOOLS)
    add_test(NAME sdk_install COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sdk_install.py
        ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_COMMAND} $<CONFIG>)
endif()

if(FLINT_PLATFORM_TESTS AND FLINT_ENABLE_LUA)
    add_executable(async_install_tests tests/async_install.cpp)
    target_link_libraries(async_install_tests PRIVATE flint_platform)
    add_test(NAME async_install COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async_install.py $<TARGET_FILE:async_install_tests>)
    set_tests_properties(async_install PROPERTIES TIMEOUT 40)
    add_executable(lua_tests tests/lua.cpp)
    target_link_libraries(lua_tests PRIVATE flint_platform)
    add_test(NAME lua_sandbox COMMAND lua_tests)
    set_tests_properties(lua_sandbox PROPERTIES TIMEOUT 10)
endif()

if(FLINT_PLATFORM_TESTS AND FLINT_SDK_TOOLS AND FLINT_ENABLE_LUA)
    add_test(NAME lua_packages COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_packages.py
        $<TARGET_FILE:ignite-check> $<TARGET_FILE:ignite-run>)
    set_tests_properties(lua_packages PROPERTIES TIMEOUT 20)
endif()

if(FLINT_PLATFORM_TESTS AND FLINT_ENABLE_LUA)
    add_executable(lua_core_tests tests/lua_core.cpp)
    target_link_libraries(lua_core_tests PRIVATE flint_platform)
    add_test(NAME lua_core COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_core.py $<TARGET_FILE:lua_core_tests>)
    set_tests_properties(lua_core PROPERTIES TIMEOUT 10)
endif()

# Keep the mixed-license scope, legal originals and operative permissions together.
install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK)
install(DIRECTORY LICENSES/ DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK/LICENSES)
install(DIRECTORY docs/ru docs/en
    DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK/docs
    FILES_MATCHING PATTERN "*.md")
install(FILES third_party/lua/lua.h third_party/lua/README.flint
    DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK/third_party/lua)
install(FILES include/flint/graphics/fonts/Lato-LICENSE.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK/third_party/Lato)

install(DIRECTORY .github/assets/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/FlintIgniteSDK/.github/assets
    FILES_MATCHING PATTERN "readme-hero*.svg")

if(FLINT_PLATFORM_TESTS AND FLINT_ENABLE_LUA)
    add_executable(app_api_tests tests/app_api.cpp)
    target_link_libraries(app_api_tests PRIVATE flint_platform)
    add_test(NAME app_api COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_app_api.py $<TARGET_FILE:app_api_tests>)
    set_tests_properties(app_api PROPERTIES TIMEOUT 20)
endif()
if(FLINT_PLATFORM_TESTS AND FLINT_SDK_TOOLS AND FLINT_ENABLE_LUA)
    add_test(NAME sdk_scenarios COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scenarios.py
        $<TARGET_FILE:ignite-run> $<TARGET_FILE:ignite-check>)
    set_tests_properties(sdk_scenarios PROPERTIES TIMEOUT 30)
endif()
