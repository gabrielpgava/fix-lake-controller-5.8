cmake_minimum_required(VERSION 3.20)
project(lake_controller_windowed_fix VERSION 0.6.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Windows x86 required. MSVC: -A Win32. MinGW: i686 toolchain.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0a00)

option(LAKE_ENABLE_PROFILING "Enable diagnostic-only bounded QPC aggregates" OFF)
add_library(LakeWindowedFix SHARED src/fix.cpp src/ddraw_scale.cpp src/profile.cpp)
if(LAKE_ENABLE_PROFILING)
    target_compile_definitions(LakeWindowedFix PRIVATE LAKE_ENABLE_PROFILING=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(LakeWindowedFix PRIVATE -fno-emulated-tls)
    endif()
endif()
set_target_properties(LakeWindowedFix PROPERTIES PREFIX "")
if(MSVC)
    target_sources(LakeWindowedFix PRIVATE src/fix-msvc.def)
else()
    target_sources(LakeWindowedFix PRIVATE src/fix.def)
endif()
target_link_libraries(LakeWindowedFix PRIVATE user32 comctl32 gdi32)
add_executable(LakeWindowedLauncher src/launcher.cpp)
target_link_libraries(LakeWindowedLauncher PRIVATE bcrypt)

function(lake_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
        get_target_property(kind ${target} TYPE)
        if(kind STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE -municode)
        endif()
    endif()
endfunction()
lake_warnings(LakeWindowedFix)
lake_warnings(LakeWindowedLauncher)

include(CTest)
if(BUILD_TESTING)
    add_library(FakeContour SHARED tests/renderer_fixture.cpp)
    set_target_properties(FakeContour PROPERTIES PREFIX "" OUTPUT_NAME "Contour")
    target_link_libraries(FakeContour PRIVATE user32 gdi32)
    add_executable(LakeFixFixture tests/fixture.cpp)
    target_link_libraries(LakeFixFixture PRIVATE user32 gdi32)
    add_executable(LakeFixInjectionTest src/launcher.cpp)
    target_compile_definitions(LakeFixInjectionTest PRIVATE LAKE_FIX_TESTING)
    target_link_libraries(LakeFixInjectionTest PRIVATE bcrypt)
    add_dependencies(LakeFixInjectionTest LakeWindowedFix LakeFixFixture FakeContour)
    add_executable(LakeFixScaleGeometryTest tests/scale_geometry_test.cpp)
    add_executable(LakeFixUiQualityTest tests/ui_quality_test.cpp)
    add_executable(LakeFixPresenterTest tests/presenter_test.cpp src/ddraw_scale.cpp src/profile.cpp)
    if(LAKE_ENABLE_PROFILING)
        target_compile_definitions(LakeFixPresenterTest PRIVATE LAKE_ENABLE_PROFILING=1)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
            target_compile_options(LakeFixPresenterTest PRIVATE -fno-emulated-tls)
        endif()
    endif()
    target_link_libraries(LakeFixPresenterTest PRIVATE user32)
    add_executable(LakeFixProfileDisabledTest tests/profile_test.cpp src/profile.cpp)
    add_executable(LakeFixProfileEnabledTest tests/profile_test.cpp src/profile.cpp)
    target_compile_definitions(LakeFixProfileEnabledTest PRIVATE LAKE_ENABLE_PROFILING=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        target_compile_options(LakeFixProfileEnabledTest PRIVATE -fno-emulated-tls)
    endif()
    lake_warnings(LakeFixScaleGeometryTest)
    lake_warnings(LakeFixUiQualityTest)
    lake_warnings(LakeFixPresenterTest)
    lake_warnings(LakeFixProfileDisabledTest)
    lake_warnings(LakeFixProfileEnabledTest)
    add_test(NAME scale_geometry COMMAND LakeFixScaleGeometryTest)
    add_test(NAME ui_quality_policy COMMAND LakeFixUiQualityTest)
    add_test(NAME scaled_presenter_objects COMMAND LakeFixPresenterTest)
    add_test(NAME profiling_disabled COMMAND LakeFixProfileDisabledTest)
    add_test(NAME profiling_enabled COMMAND LakeFixProfileEnabledTest)
    add_test(NAME resource_metrics_policy COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Resource-Metrics.Test.ps1")
    add_test(NAME isolated_build_options COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Build-Options.Test.ps1")
    lake_warnings(FakeContour)
    lake_warnings(LakeFixFixture)
    lake_warnings(LakeFixInjectionTest)
    add_test(NAME windows_safe_no_rebuild COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-Fixture.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:LakeWindowedFix>" -NativeResize 0 -Profiling "$<BOOL:${LAKE_ENABLE_PROFILING}>")
    add_test(NAME windows_experimental_resize COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-Fixture.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:LakeWindowedFix>" -NativeResize 1 -Profiling "$<BOOL:${LAKE_ENABLE_PROFILING}>")
    add_test(NAME windows_exit_observer COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-Fixture.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:LakeWindowedFix>" -NativeResize 0 -ExpectedExit 23 -Profiling "$<BOOL:${LAKE_ENABLE_PROFILING}>")
    add_test(NAME windows_scaled_window COMMAND powershell.exe -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/Run-Fixture.ps1"
        -BinaryDirectory "$<TARGET_FILE_DIR:LakeWindowedFix>" -NativeResize 0 -ScaleRendering -Profiling "$<BOOL:${LAKE_ENABLE_PROFILING}>")
    set_tests_properties(windows_safe_no_rebuild windows_experimental_resize windows_exit_observer windows_scaled_window
        PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()

install(TARGETS LakeWindowedFix LakeWindowedLauncher RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES LICENSE DESTINATION .)
install(FILES scripts/Measure-LakeResources.ps1 scripts/LakeResourceMetrics.psm1 DESTINATION .)

# Optional self-contained, per-user GUI installer. The ordinary runtime build
# and its validated DLL/INI do not depend on the packaging target.
option(LAKE_BUILD_SETUP "Build the one-button per-user installer" OFF)
if(LAKE_BUILD_SETUP AND LAKE_ENABLE_PROFILING)
    message(FATAL_ERROR "Diagnostic profiling must be disabled for installer builds.")
endif()
if(LAKE_BUILD_SETUP)
    set(setup_payload "${CMAKE_CURRENT_BINARY_DIR}/setup-payload/$<CONFIG>/installer_payload.h")
    add_custom_command(OUTPUT "${setup_payload}"
        COMMAND powershell.exe -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/scripts/New-InstallerPayload.ps1"
            -RuntimeDirectory "$<TARGET_FILE_DIR:LakeWindowedFix>"
            -ConfigurationDirectory "${CMAKE_CURRENT_SOURCE_DIR}/config"
            -SupportDirectory "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
            -LicensePath "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            -OutputHeader "${setup_payload}"
        DEPENDS LakeWindowedFix LakeWindowedLauncher
            config/LakeWindowedFix.ini LICENSE scripts/Measure-LakeResources.ps1
            scripts/LakeResourceMetrics.psm1 scripts/New-InstallerPayload.ps1
        VERBATIM)
    if(MSVC)
        enable_language(RC)
        set(setup_manifest src/setup.rc)
    else()
        enable_language(ASM)
        set(setup_manifest src/setup_manifest.S)
    endif()
    add_executable(LakeWindowedSetup WIN32 src/setup.cpp src/setup_engine.cpp "${setup_payload}" ${setup_manifest})
    set_target_properties(LakeWindowedSetup PROPERTIES OUTPUT_NAME "LakeWindowedFix-Setup-v0.6")
    target_include_directories(LakeWindowedSetup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/setup-payload/$<CONFIG>")
    target_link_libraries(LakeWindowedSetup PRIVATE bcrypt user32 gdi32 ole32 shell32 advapi32 uuid)
    lake_warnings(LakeWindowedSetup)
    install(TARGETS LakeWindowedSetup RUNTIME DESTINATION installer)
    if(BUILD_TESTING)
        add_executable(LakeFixSetupTest tests/setup_engine_test.cpp src/setup_engine.cpp ${setup_manifest})
        target_link_libraries(LakeFixSetupTest PRIVATE bcrypt user32 gdi32 ole32 shell32 advapi32 uuid)
        lake_warnings(LakeFixSetupTest)
        add_test(NAME installer_sandbox COMMAND LakeFixSetupTest)
        set_tests_properties(installer_sandbox PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
