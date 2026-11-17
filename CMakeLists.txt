cmake_minimum_required(VERSION 3.10)
project(w5500_async_driver VERSION 0.6.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(W5500_MINIMAL "Build only the chip core and socket layer" OFF)
option(W5500_BUILD_TESTS "Build and register the host unit tests" ON)
option(W5500_BUILD_HOST_DEMOS "Build and register the emulator demos" ON)

set(W5500_CORE_SRC
    src/w5500.c
    src/w5500_sockets.c)
set(W5500_PROTO_SRC
    src/w5500_dhcp.c
    src/w5500_dns.c
    src/w5500_sntp.c
    src/w5500_modbus.c
    src/w5500_mqtt.c
    src/w5500_http.c)

if(W5500_MINIMAL)
    set(W5500_SRC ${W5500_CORE_SRC})
else()
    set(W5500_SRC ${W5500_CORE_SRC} ${W5500_PROTO_SRC})
endif()

add_library(w5500 STATIC ${W5500_SRC})
target_include_directories(w5500 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(w5500 PRIVATE src)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(w5500 PRIVATE -Wall -Wextra -Werror)
endif()

if(NOT W5500_BUILD_TESTS AND NOT W5500_BUILD_HOST_DEMOS)
    return()
endif()

enable_testing()
add_library(w5500_mock STATIC tests/mock_w5500.c)
target_include_directories(w5500_mock PUBLIC include src tests)
target_link_libraries(w5500_mock PUBLIC w5500)

function(w5500_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE include src tests)
    target_link_libraries(${name} PRIVATE w5500_mock)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(W5500_BUILD_TESTS)
    w5500_add_test(test_w5500 tests/test_w5500.c)
    w5500_add_test(test_protocols tests/test_protocols.c)
    w5500_add_test(test_modbus tests/test_modbus.c)
    w5500_add_test(test_http tests/test_http.c)
    w5500_add_test(test_mqtt tests/test_mqtt.c)
    w5500_add_test(test_dns_ex tests/test_dns_ex.c)
    w5500_add_test(test_stats tests/test_stats.c)
endif()

if(W5500_BUILD_HOST_DEMOS)
    w5500_add_test(host_mock_demo examples/host_mock_demo/main.c)
    w5500_add_test(host_http_demo examples/host_http_demo/main.c)
endif()
