cmake_minimum_required(VERSION 3.22)

project(devdatapod
    VERSION 1.0.1
    DESCRIPTION "Green portable local dev service panel"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Qml Quick QuickWidgets Svg)
qt_standard_project_setup()

# PowerShell is not the only ANSI trap here: without /utf-8, MSVC reads these
# UTF-8 Chinese strings as the system code page and emits C4819 (AGENTS.md rule 9).
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# ---------------------------------------------------------------- design tokens
# Regenerated from design/devconsole.pen at configure time when the pen source is present.
# The generated files under src/ui/theme/generated are committed; without the design
# source the build reuses them as-is. `node scripts/gen-theme.mjs` regenerates manually.
find_program(NODE_EXECUTABLE NAMES node node.exe)
set(THEME_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/theme/generated)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/design/devconsole.pen)
    if(NOT NODE_EXECUTABLE)
        message(FATAL_ERROR
            "node is required to generate the theme from design/devconsole.pen "
            "(install Node.js, or run `node scripts/gen-theme.mjs` and keep the output dir)")
    endif()
    execute_process(
        COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-theme.mjs
        RESULT_VARIABLE theme_gen_rc
        OUTPUT_VARIABLE theme_gen_out
        ERROR_VARIABLE theme_gen_err
    )
    if(NOT theme_gen_rc EQUAL 0)
        message(FATAL_ERROR "gen-theme.mjs failed:\n${theme_gen_out}${theme_gen_err}")
    endif()
    message(STATUS "theme tokens: ${theme_gen_out}")
    set_source_files_properties(
        ${THEME_GEN_DIR}/tokens.json ${THEME_GEN_DIR}/theme.qss ${THEME_GEN_DIR}/Tokens.qml
        PROPERTIES GENERATED TRUE
    )
else()
    message(STATUS "theme tokens: design/devconsole.pen not found, reusing committed generated files")
endif()

# ---------------------------------------------------------------------- spdlog
include(FetchContent)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(spdlog)

# ------------------------------------------------------------------------本体
# Two targets from one source list. The panel is the GUI executable; devdatapod-selftest is a
# console executable that links the core group *without* Qt6::Widgets/QuickWidgets. That is
# what makes AGENTS.md rule 3 mechanical: the day a file under src/core includes a widget
# header, the selftest target stops linking instead of the rule quietly rotting.
set(DEVDATAPOD_CORE_SOURCES
    src/core/util/AppPaths.h
    src/core/util/AppPaths.cpp
    src/core/util/SettingsStore.h
    src/core/util/SettingsStore.cpp
    src/core/util/Log.h
    src/core/util/Log.cpp
    src/core/util/AtomicFile.h
    src/core/util/AtomicFile.cpp
    src/core/util/LogTailReader.h
    src/core/util/LogTailReader.cpp
    src/core/util/Unzip.h
    src/core/util/Unzip.cpp
    src/core/model/RunState.h
    src/core/net/PortAllocator.h
    src/core/net/PortAllocator.cpp
    src/core/net/PortProbe.h
    src/core/net/PortProbe.cpp
    src/core/net/Downloader.h
    src/core/net/Downloader.cpp
    src/core/process/Supervisor.h
    src/core/process/Supervisor.cpp
    src/core/process/SystemStat.h
    src/core/process/SystemStat.cpp
    src/core/process/ProcessStat.h
    src/core/process/ProcessStat.cpp
    src/core/process/RunOnce.h
    src/core/process/RunOnce.cpp
    src/core/process/OrphanSweep.h
    src/core/process/OrphanSweep.cpp
    src/core/config/ManagedBlock.h
    src/core/config/ManagedBlock.cpp
    src/core/service/ServiceKind.h
    src/core/service/ServiceKind.cpp
    src/core/service/ComponentCatalog.h
    src/core/service/ComponentCatalog.cpp
    src/core/service/ServiceInstance.h
    src/core/service/ServiceInstance.cpp
    src/core/service/InstanceRegistry.h
    src/core/service/InstanceRegistry.cpp
    src/core/service/IServiceDriver.h
    src/core/service/RedisDriver.h
    src/core/service/RedisDriver.cpp
    src/core/service/MysqlDriver.h
    src/core/service/MysqlDriver.cpp
    src/core/service/NginxDriver.h
    src/core/service/NginxDriver.cpp
    src/core/service/SiteStore.h
    src/core/service/SiteStore.cpp
    src/core/service/Engine.h
    src/core/service/Engine.cpp
    src/core/service/DefaultSwitcher.h
    src/core/service/DefaultSwitcher.cpp
    src/app/SelfTest.h
    src/app/SelfTest.cpp
    src/app/Stage.h
    src/app/Stage.cpp
)

set(DEVDATAPOD_UI_SOURCES
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/ServiceOperator.h
    src/ui/ServiceOperator.cpp
    src/ui/InstallOperator.h
    src/ui/InstallOperator.cpp
    src/ui/theme/Theme.h
    src/ui/theme/Theme.cpp
    src/ui/theme/IconFactory.h
    src/ui/theme/IconFactory.cpp
    src/ui/widgets/UiFactory.h
    src/ui/widgets/UiFactory.cpp
    src/ui/widgets/ConfirmDialog.h
    src/ui/widgets/ConfirmDialog.cpp
    src/ui/widgets/ConfigDialog.h
    src/ui/widgets/ConfigDialog.cpp
    src/ui/widgets/StatusDot.h
    src/ui/widgets/StatusDot.cpp
    src/ui/widgets/TitleBar.h
    src/ui/widgets/TitleBar.cpp
    src/ui/widgets/Sidebar.h
    src/ui/widgets/Sidebar.cpp
    src/ui/widgets/StatusBarPanel.h
    src/ui/widgets/StatusBarPanel.cpp
    src/ui/widgets/ServiceRow.h
    src/ui/widgets/ServiceRow.cpp
    src/ui/widgets/RoleIndicator.h
    src/ui/widgets/RoleIndicator.cpp
    src/ui/widgets/ToggleSwitch.h
    src/ui/widgets/ToggleSwitch.cpp
    src/ui/widgets/GaugeGroup.h
    src/ui/widgets/GaugeGroup.cpp
    src/ui/widgets/MeterBar.h
    src/ui/widgets/MeterBar.cpp
    src/ui/widgets/pages/HomePage.h
    src/ui/widgets/pages/HomePage.cpp
    src/ui/widgets/pages/SitePage.h
    src/ui/widgets/pages/SitePage.cpp
    src/ui/widgets/pages/SoftwarePage.h
    src/ui/widgets/pages/SoftwarePage.cpp
    src/ui/widgets/pages/SettingsPage.h
    src/ui/widgets/pages/SettingsPage.cpp
    src/ui/widgets/pages/PlaceholderPage.h
    src/ui/widgets/pages/PlaceholderPage.cpp
    src/ui/widgets/SiteDialog.h
    src/ui/widgets/SiteDialog.cpp
)

# exe 图标：RC 编译器在 build 目录工作，把 ico 复制过去让它能找到。
configure_file(assets/logo.ico ${CMAKE_CURRENT_BINARY_DIR}/logo.ico COPYONLY)

qt_add_executable(devdatapod
    src/app/main.cpp
    src/app/SingleInstance.h
    src/app/SingleInstance.cpp
    src/app/TrayController.h
    src/app/TrayController.cpp
    assets/app.rc
    ${DEVDATAPOD_CORE_SOURCES}
    ${DEVDATAPOD_UI_SOURCES}
)

# Console subsystem on purpose: `devdatapod.exe --selftest` cannot report an exit code to a shell
# that does not wait for a windowed process, and P2's evidence is that exit code.
qt_add_executable(devdatapod-selftest
    src/app/selftest_main.cpp
    ${DEVDATAPOD_CORE_SOURCES}
)

target_include_directories(devdatapod PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(devdatapod-selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(devdatapod PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickWidgets Qt6::Svg
    spdlog::spdlog_header_only
)

target_link_libraries(devdatapod-selftest PRIVATE
    # Deliberately Core + Network only: no Gui, no Widgets, no Qml. If a core file ever
    # includes a painting or widget header, this target stops building.
    Qt6::Core Qt6::Network
    spdlog::spdlog_header_only
)

if(WIN32)
    # GetExtendedTcpTable / QueryFullProcessImageNameW: port -> owning process.
    target_link_libraries(devdatapod PRIVATE iphlpapi)
    target_link_libraries(devdatapod-selftest PRIVATE iphlpapi)
endif()

foreach(target devdatapod devdatapod-selftest)
    target_compile_definitions(${target} PRIVATE
        DEVDATAPOD_VERSION="${PROJECT_VERSION}"
        $<$<CONFIG:Debug>:DEVDATAPOD_DEBUG>
        # filename_t becomes std::wstring, so D:\小皮面板\ still opens a log file.
        $<$<PLATFORM_ID:Windows>:SPDLOG_WCHAR_FILENAMES>
    )
endforeach()

qt_add_resources(devdatapod "devdatapod_assets"
    PREFIX "/devdatapod"
    BASE "src"
    FILES
        src/ui/theme/generated/tokens.json
        src/ui/theme/generated/theme.qss
        src/ui/theme/generated/Tokens.qml
        src/ui/qml/Gauges.qml
        src/ui/theme/fonts/MiSans-Regular.ttf
        src/ui/theme/fonts/MiSans-Medium.ttf
        src/ui/theme/icons/default-on.svg
        src/ui/theme/icons/default-off.svg
)

qt_add_resources(devdatapod "devdatapod_logo"
    PREFIX "/devdatapod"
    BASE "."
    FILES
        assets/logo.svg
)

# Shipped inside the binary so a fresh install can seed data/ports.json without a
# config/ folder next to the exe (AGENTS.md rule 7: relative, portable layout). The
# selftest needs it for the same reason: it seeds a missing ports.json in a temp root.
foreach(target devdatapod devdatapod-selftest)
    qt_add_resources(${target} "devdatapod_config"
        PREFIX "/devdatapod"
        BASE "."
        FILES
            config/ports.default.json
    )
endforeach()

if(WIN32)
    set_target_properties(devdatapod PROPERTIES WIN32_EXECUTABLE TRUE)
    # FALSE is the default; spelled out because the whole reason this target is separate
    # from `devdatapod --selftest` is that a console program makes cmd and PowerShell wait.
    set_target_properties(devdatapod-selftest PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
