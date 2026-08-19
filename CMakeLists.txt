cmake_minimum_required(VERSION 3.20)
project(ydPenetrationTest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(yd_test_core
    src/core/Config.cpp
    src/core/Logger.cpp
    src/core/TestResult.cpp
    src/core/Monitor.cpp
    src/core/OrderValidator.cpp
    src/core/YdSession.cpp
    src/tests/TestCases.cpp
)
target_include_directories(yd_test_core PUBLIC include ydapi/include)

if(WIN32)
    target_link_libraries(yd_test_core PUBLIC "${CMAKE_SOURCE_DIR}/ydapi/win64/yd.lib")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(yd_test_core PUBLIC "${CMAKE_SOURCE_DIR}/ydapi/linux64/yd.so" Threads::Threads dl rt)
    set_target_properties(yd_test_core PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

function(add_yd_test name)
    add_executable(${name} apps/${name}.cpp)
    target_link_libraries(${name} PRIVATE yd_test_core)
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
    if(WIN32)
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/ydapi/win64/yd.dll"
                "$<TARGET_FILE_DIR:${name}>/yd.dll")
    else()
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/ydapi/linux64/yd.so"
                "$<TARGET_FILE_DIR:${name}>/yd.so")
        set_target_properties(${name} PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
endfunction()

add_yd_test(test_01_connect)
add_yd_test(test_12_market_position)
add_yd_test(test_02_basic_trade)
add_yd_test(test_03_reconnect)
add_yd_test(test_04_order_cancel_count)
add_yd_test(test_05_duplicate_order)
add_yd_test(test_06_threshold)
add_yd_test(test_07_instruction_check)
add_yd_test(test_08_error_message)
add_yd_test(test_09_pause_trade)
add_yd_test(test_10_batch_cancel)
add_yd_test(test_11_logging)
add_yd_test(test_all)
add_yd_test(test_list_instruments)
