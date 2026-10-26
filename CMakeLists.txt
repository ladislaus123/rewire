cmake_minimum_required(VERSION 3.20)
project(rewire LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_library(LIBCLANG_LIBRARY
    NAMES clang
    PATHS /Library/Developer/CommandLineTools/usr/lib /usr/lib /usr/local/lib
    REQUIRED)
find_package(Threads REQUIRED)

if(EXISTS "/opt/homebrew/opt/qt@5/lib/cmake/Qt5/Qt5Config.cmake")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/qt@5")
elseif(EXISTS "/usr/local/opt/qt@5/lib/cmake/Qt5/Qt5Config.cmake")
    list(APPEND CMAKE_PREFIX_PATH "/usr/local/opt/qt@5")
endif()
find_package(Qt5 REQUIRED COMPONENTS Widgets Test)

include(FetchContent)
FetchContent_Declare(ftxui
    URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v5.0.0.tar.gz)
FetchContent_MakeAvailable(ftxui)

add_library(rewire_core
    src/json.cpp
    src/model.cpp
    src/parser.cpp
    src/text_lang.cpp
    src/diagram.cpp
    src/claude_bridge.cpp
    src/watcher.cpp
    src/edit.cpp)
target_include_directories(rewire_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(rewire_core PUBLIC ${LIBCLANG_LIBRARY} Threads::Threads)
target_compile_options(rewire_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_library(rewire_gui STATIC src/gui_window.cpp)
target_link_libraries(rewire_gui PUBLIC rewire_core Qt5::Widgets)
target_include_directories(rewire_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(rewire_gui PROPERTIES AUTOMOC ON)
target_compile_options(rewire_gui PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(rewire src/main.cpp src/app.cpp src/gui_app.cpp)
target_link_libraries(rewire PRIVATE rewire_gui ftxui::screen ftxui::dom ftxui::component)
target_compile_options(rewire PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(rewire_test tests/test_main.cpp)
target_link_libraries(rewire_test PRIVATE rewire_core)
target_compile_definitions(rewire_test PRIVATE REWIRE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(rewire_test PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(bookstore_demo
    examples/bookstore/src/author.cpp
    examples/bookstore/src/book.cpp
    examples/bookstore/src/money.cpp
    examples/bookstore/src/logger.cpp
    examples/bookstore/src/payment.cpp
    examples/bookstore/src/order.cpp
    examples/bookstore/src/promotion.cpp
    examples/bookstore/src/order_repository.cpp
    examples/bookstore/src/order_service.cpp
    examples/bookstore/src/catalog.cpp
    examples/bookstore/src/demo.cpp)
target_include_directories(bookstore_demo PRIVATE examples/bookstore/include)
target_compile_options(bookstore_demo PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(rewire_gui_test tests/gui_smoke.cpp)
target_link_libraries(rewire_gui_test PRIVATE rewire_gui Qt5::Test)
target_compile_definitions(rewire_gui_test PRIVATE REWIRE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_options(rewire_gui_test PRIVATE -Wall -Wextra -Wno-unused-parameter)

enable_testing()
add_test(NAME rewire_test COMMAND rewire_test)
add_test(NAME rewire_gui_test COMMAND rewire_gui_test)
set_tests_properties(rewire_gui_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
