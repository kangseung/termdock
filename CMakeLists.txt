cmake_minimum_required(VERSION 3.16)
project(termdock VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# Windows 上 socket API 在 ws2_32 里，不链接就全部 undefined（select/closesocket/getaddrinfo）
if(WIN32)
    set(TERMDOCK_PLATFORM_LIBS ws2_32)
else()
    set(TERMDOCK_PLATFORM_LIBS "")
endif()

find_path(LIBSSH2_INCLUDE_DIR libssh2.h)
find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2)
if(NOT LIBSSH2_INCLUDE_DIR OR NOT LIBSSH2_LIBRARY)
    message(FATAL_ERROR "libssh2 not found (install libssh2-1-dev / libssh2-devel)")
endif()

set(TERMDOCK_SOURCES
    src/app/AppConfig.cpp
    src/app/Theme.cpp
    src/session/Crypto.cpp
    src/session/Recovery.cpp
    src/session/Session.cpp
    src/session/SessionStore.cpp
    src/term/VtScreen.cpp
    src/term/VtParser.cpp
    src/term/TerminalWidget.cpp
    src/ssh/NetCompat.cpp
    src/ssh/SshConnection.cpp
    src/ssh/ShellSession.cpp
    src/ssh/SftpWorker.cpp
    src/ssh/SftpModel.cpp
    src/ui/MainWindow.cpp
    src/ui/SessionPanel.cpp
    src/ui/SessionDialog.cpp
    src/ui/FileTreePanel.cpp
    src/ui/DataDirDialog.cpp
    src/ui/VaultDialog.cpp
    src/main.cpp
)

set(TERMDOCK_HEADERS
    src/app/AppConfig.h
    src/app/Theme.h
    src/session/Crypto.h
    src/session/Recovery.h
    src/session/Session.h
    src/session/SessionStore.h
    src/term/VtScreen.h
    src/term/VtParser.h
    src/term/TerminalWidget.h
    src/ssh/NetCompat.h
    src/ssh/SshConnection.h
    src/ssh/ShellSession.h
    src/ssh/SftpWorker.h
    src/ssh/SftpModel.h
    src/ui/MainWindow.h
    src/ui/SessionPanel.h
    src/ui/SessionDialog.h
    src/ui/FileTreePanel.h
    src/ui/DataDirDialog.h
    src/ui/VaultDialog.h
)

set(CMAKE_AUTORCC ON)

# WIN32 是必需的：否则 Windows 上会额外弹一个控制台黑框
add_executable(termdock WIN32 ${TERMDOCK_SOURCES} ${TERMDOCK_HEADERS} termdock.qrc)

target_include_directories(termdock PRIVATE src ${LIBSSH2_INCLUDE_DIR})
# 版本号只在 CMakeLists 里维护：以前 main.cpp 手抄一份，改了 CMake 忘了改它，
# 结果 0.2.1 的包 --version 还报 0.1.5
target_compile_definitions(termdock PRIVATE TERMDOCK_VERSION="${PROJECT_VERSION}")
target_link_libraries(termdock PRIVATE
    Qt6::Widgets
    OpenSSL::Crypto
    ${LIBSSH2_LIBRARY}
    Threads::Threads
    ${TERMDOCK_PLATFORM_LIBS}
)

if(MSVC)
    target_compile_options(termdock PRIVATE /W3)
else()
    target_compile_options(termdock PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# 便携包把 Qt 库放在同级的 lib/ 下，运行时按 $ORIGIN/lib 找（系统安装时该目录不存在，
# 自然回退到系统 Qt，不影响 deb 安装版）
if(UNIX AND NOT APPLE)
    set_target_properties(termdock PROPERTIES
        BUILD_RPATH "\$ORIGIN/lib"
        INSTALL_RPATH "\$ORIGIN/lib")
endif()

# Windows 上把运行库和 Qt 插件直接摆到 exe 旁边：否则 `build/` 里的 exe 双击就弹
# 「找不到 Qt6Core.dll」，构建产物本身没法直接运行/自检，很容易被误当成程序坏了。
#
# 用 packaging/deploy-windows-deps.sh 而不是直接调 windeployqt：后者只搬 Qt 自己的
# 直接依赖，Qt6Gui 背后的 icu/freetype/harfbuzz/glib 那串传递依赖不会被递归带上，
# 双击照样启动失败。发布用的便携包也用同一个脚本，保证两边一致。
if(WIN32)
    find_program(TERMDOCK_BASH NAMES bash HINTS "C:/msys64/usr/bin" "C:/msys64/bin")
    if(TERMDOCK_BASH)
        add_custom_command(TARGET termdock POST_BUILD
            COMMAND "${TERMDOCK_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/deploy-windows-deps.sh"
                "$<TARGET_FILE_DIR:termdock>" termdock
            COMMENT "部署 Qt 与运行库到构建目录（让编译产物能直接双击运行）"
            VERBATIM)
    else()
        message(STATUS "未找到 bash：build/ 里的 exe 不会自带 Qt DLL，"
                       "请从 dist/ 的便携包运行，或用 packaging/make-release-windows.sh 打包")
    endif()
endif()

install(TARGETS termdock RUNTIME DESTINATION bin)
# .desktop 和 hicolor 图标是 Linux 桌面规范，Windows 上装了也没意义
if(UNIX AND NOT APPLE)
    install(FILES packaging/termdock.desktop DESTINATION share/applications)
    install(FILES assets/icons/termdock-16.png DESTINATION share/icons/hicolor/16x16/apps RENAME termdock.png)
    install(FILES assets/icons/termdock-32.png DESTINATION share/icons/hicolor/32x32/apps RENAME termdock.png)
    install(FILES assets/icons/termdock-64.png DESTINATION share/icons/hicolor/64x64/apps RENAME termdock.png)
    install(FILES assets/icons/termdock-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME termdock.png)
endif()

# ---------------- 单元测试 ----------------
enable_testing()

add_executable(termdock-tests
    tests/unit_tests.cpp
    src/term/VtScreen.cpp
    src/term/VtParser.cpp
    src/session/Crypto.cpp
    src/session/Recovery.cpp
    src/session/Session.cpp
    src/session/SessionStore.cpp
    src/app/AppConfig.cpp
)
target_include_directories(termdock-tests PRIVATE src)
target_link_libraries(termdock-tests PRIVATE
    Qt6::Gui
    Qt6::Core
    OpenSSL::Crypto
    ${TERMDOCK_PLATFORM_LIBS}
)
add_test(NAME termdock-tests COMMAND termdock-tests)
