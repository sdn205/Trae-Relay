cmake_minimum_required(VERSION 3.20)
project(TraeRelay VERSION 0.11.0 LANGUAGES CXX)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)

# 版本号唯一来源：修改这里后，命令行、User-Agent 和 Windows 清单会一起更新。
set(TRAE_VERSION_DISPLAY "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
configure_file(${SRC}/app/Version.h.in ${GEN}/Version.h @ONLY)
configure_file(${SRC}/res/app.manifest.in ${GEN}/app.manifest @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # /MT 静态链接

set(TRAE_SOURCES
    ${SRC}/main.cpp
    ${SRC}/app/AutoStart.cpp
    ${SRC}/app/Config.cpp
    ${SRC}/app/Service.cpp
    ${SRC}/window/MainWindow.cpp
    ${SRC}/window/ModelsPage.cpp
    ${SRC}/window/SettingsPage.cpp
    ${SRC}/window/StatusPage.cpp
    ${SRC}/window/UsagePage.cpp
    ${SRC}/window/WindowChrome.cpp
    ${SRC}/window/WindowController.cpp
    ${SRC}/window/WindowEvents.cpp
    ${SRC}/window/WindowLayout.cpp
    ${SRC}/window/WindowSnapshot.cpp
    ${SRC}/window/WindowTasks.cpp
    ${SRC}/window/WindowTray.cpp
    ${SRC}/ui/Dpi.cpp
    ${SRC}/ui/UiControls.cpp
    ${SRC}/ui/UiDrawing.cpp
    ${SRC}/ui/UiLayout.cpp
    ${SRC}/ui/UiUsageTable.cpp
    ${SRC}/api/Facade.cpp
    ${SRC}/api/ResponsesApi.cpp
    ${SRC}/accounts/AccountPool.cpp
    ${SRC}/accounts/Storage.cpp
    ${SRC}/accounts/TokenRefresh.cpp
    ${SRC}/upstream/ModelCatalog.cpp
    ${SRC}/upstream/ReasoningEffort.cpp
    ${SRC}/upstream/Upstream.cpp
    ${SRC}/upstream/UpstreamSolo.cpp
    ${SRC}/common/Crypto.cpp
    ${SRC}/common/Http.cpp
    ${SRC}/common/HttpServer.cpp
    ${SRC}/common/Json.cpp
    ${SRC}/common/Log.cpp
    ${SRC}/common/Settings.cpp
    ${SRC}/common/SseParser.cpp
    ${SRC}/common/Stats.cpp
    ${SRC}/res/app.ico
    ${GEN}/app.manifest
    ${SRC}/res/app.rc
)

set(TRAE_HEADERS
    ${INC}/app/AutoStart.h
    ${INC}/app/Config.h
    ${INC}/app/ConfigSchema.h
    ${INC}/app/Service.h
    ${INC}/window/MainWindow.h
    ${INC}/window/WindowController.h
    ${INC}/ui/Dpi.h
    ${INC}/ui/UiControls.h
    ${INC}/ui/UiDrawing.h
    ${INC}/ui/UiLayout.h
    ${INC}/ui/UiStyle.h
    ${INC}/ui/UiUsageTable.h
    ${INC}/api/Facade.h
    ${INC}/accounts/AccountPool.h
    ${INC}/accounts/Storage.h
    ${INC}/accounts/TokenRefresh.h
    ${INC}/upstream/ModelCatalog.h
    ${INC}/upstream/ReasoningEffort.h
    ${INC}/upstream/Upstream.h
    ${INC}/common/Crypto.h
    ${INC}/common/Http.h
    ${INC}/common/HttpServer.h
    ${INC}/common/Json.h
    ${INC}/common/Log.h
    ${INC}/common/Settings.h
    ${INC}/common/SseParser.h
    ${INC}/common/Stats.h
    ${GEN}/Version.h
)

add_executable(TraeRelay ${TRAE_SOURCES} ${TRAE_HEADERS})

# Visual Studio 按实际 src/include 目录展示源码与头文件。
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${TRAE_SOURCES} ${TRAE_HEADERS})

# rc.exe 资源数据文件（app.ico）相对 rc 所在目录搜索
set_source_files_properties(${SRC}/res/app.rc PROPERTIES
    COMPILE_FLAGS "/I \"${SRC}/res\"")

target_include_directories(TraeRelay PRIVATE ${INC})
target_include_directories(TraeRelay PRIVATE ${GEN})
target_compile_options(TraeRelay PRIVATE /utf-8)
target_compile_definitions(TraeRelay PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS SECURITY_WIN32 _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(TraeRelay PRIVATE bcrypt winhttp ws2_32 secur32 shlwapi comctl32 shell32 user32 gdi32 advapi32 ole32 crypt32 d2d1 dwrite windowscodecs shcore dwmapi uxtheme)

# GUI 子系统但保留 main 入口（无控制台窗口）
if(MSVC)
    target_link_options(TraeRelay PRIVATE "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")
endif()

# 产物直接落到 dist/
set(DIST "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set_target_properties(TraeRelay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DIST}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DIST}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DIST}"
)
