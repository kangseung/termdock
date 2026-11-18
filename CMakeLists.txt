cmake_minimum_required(VERSION 3.16)
project(termdock VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

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

add_executable(termdock ${TERMDOCK_SOURCES} ${TERMDOCK_HEADERS} termdock.qrc)

target_include_directories(termdock PRIVATE src ${LIBSSH2_INCLUDE_DIR})
target_link_libraries(termdock PRIVATE
    Qt6::Widgets
    OpenSSL::Crypto
    ${LIBSSH2_LIBRARY}
    Threads::Threads
)

if(MSVC)
    target_compile_options(termdock PRIVATE /W3)
else()
    target_compile_options(termdock PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS termdock RUNTIME DESTINATION bin)
install(FILES packaging/termdock.desktop DESTINATION share/applications)
install(FILES assets/icons/termdock-16.png DESTINATION share/icons/hicolor/16x16/apps RENAME termdock.png)
install(FILES assets/icons/termdock-32.png DESTINATION share/icons/hicolor/32x32/apps RENAME termdock.png)
install(FILES assets/icons/termdock-64.png DESTINATION share/icons/hicolor/64x64/apps RENAME termdock.png)
install(FILES assets/icons/termdock-256.png DESTINATION share/icons/hicolor/256x256/apps RENAME termdock.png)

# ---------------- 单元测试 ----------------
enable_testing()

add_executable(termdock-tests
    tests/unit_tests.cpp
    src/term/VtScreen.cpp
    src/term/VtParser.cpp
    src/session/Crypto.cpp
    src/session/Recovery.cpp
    src/session/Session.cpp
    src/app/AppConfig.cpp
)
target_include_directories(termdock-tests PRIVATE src)
target_link_libraries(termdock-tests PRIVATE Qt6::Gui Qt6::Core OpenSSL::Crypto)
add_test(NAME termdock-tests COMMAND termdock-tests)
