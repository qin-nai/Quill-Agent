# 交叉编译 Hermes native .so:复用 core 源码(引擎+server 零改动),
# 仅入口(server_android.cpp)与出站传输(http_transport_android.cpp)为 Android 专用。
cmake_minimum_required(VERSION 3.22)
project(hermes_native LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core)

add_library(hermes_native SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/http_transport_android.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/server_android.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/platform_thread_android.cpp
    ${CORE_DIR}/src/llm/claude_adapter.cpp
    ${CORE_DIR}/src/llm/openai_compat_adapter.cpp
    ${CORE_DIR}/src/llm/stub_adapter.cpp
    ${CORE_DIR}/src/provider.cpp
    ${CORE_DIR}/src/agent_loop.cpp
    ${CORE_DIR}/server/app_context.cpp
    ${CORE_DIR}/server/session_store.cpp
    ${CORE_DIR}/server/keystore.cpp
    ${CORE_DIR}/server/model_store.cpp
    ${CORE_DIR}/server/run_context.cpp
    ${CORE_DIR}/server/mcp_client.cpp
    ${CORE_DIR}/server/tool_factory.cpp
    ${CORE_DIR}/server/routes.cpp
    ${CORE_DIR}/server/run_script_tool.cpp
    ${CORE_DIR}/server/run_script_bridge.c
    ${CORE_DIR}/third_party/sqlite3/sqlite3.c
    ${CORE_DIR}/third_party/quickjs/quickjs.c
    ${CORE_DIR}/third_party/quickjs/cutils.c
    ${CORE_DIR}/third_party/quickjs/libregexp.c
    ${CORE_DIR}/third_party/quickjs/libunicode.c
    ${CORE_DIR}/third_party/quickjs/dtoa.c
)

target_include_directories(hermes_native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CORE_DIR}/include
    ${CORE_DIR}/third_party
    ${CORE_DIR}/third_party/httplib
    ${CORE_DIR}/third_party/sqlite3
    ${CORE_DIR}/third_party/quickjs
    ${CORE_DIR}/src
    ${CORE_DIR}/server
)

find_library(log-lib log)
target_link_libraries(hermes_native ${log-lib})
