cmake_minimum_required(VERSION 3.16)
project(hermes-core LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # 源码为 UTF-8(含中文注释/字符串),必须显式声明,否则 MSVC 按本机代码页误读
    add_compile_options(/utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

# 纯 C++ Core:不依赖平台。M2 起通过注入的 HttpTransport 接真实网络(cpp-httplib + TLS)。
add_library(hermes_core
    src/llm/claude_adapter.cpp
    src/llm/openai_compat_adapter.cpp
    src/llm/stub_adapter.cpp
    src/provider.cpp
    src/agent_loop.cpp
)
target_include_directories(hermes_core PUBLIC include)
# nlohmann/json 是 header-only 且被公开头文件引用,须 PUBLIC 传播给消费者
target_include_directories(hermes_core PUBLIC third_party)

add_executable(hermes_cli cli/main.cpp)
target_link_libraries(hermes_cli PRIVATE hermes_core)

# QuickJS 沙盒:Windows 的 MSVC C 无法编译 QuickJS(NaN-boxing 的 JSValue cast 系统性不兼容),
# 用降级实现;Android(clang)编译完整 QuickJS。run_script_tool.cpp 两平台共享。
if(WIN32)
    set(RUN_SCRIPT_BRIDGE server/run_script_bridge_stub.c)
    set(QUICKJS_SOURCES "")
else()
    set(RUN_SCRIPT_BRIDGE server/run_script_bridge.c)
    set(QUICKJS_SOURCES
        third_party/quickjs/quickjs.c third_party/quickjs/cutils.c
        third_party/quickjs/libregexp.c third_party/quickjs/libunicode.c
        third_party/quickjs/dtoa.c)
endif()

# HTTP server:入站 cpp-httplib,出站 WinHTTP,持久化 SQLite(vendored amalgamation)。
# SQLite 是 C 代码,须 project(LANGUAGES ... C) 以 C 编译器编译。
add_executable(hermes_server
    server/main.cpp
    server/app_context.cpp
    server/session_store.cpp
    server/keystore.cpp
    server/model_store.cpp
    server/run_context.cpp
    server/platform_thread_windows.cpp
    server/http_transport_winhttp.cpp
    server/mcp_client.cpp
    server/tool_factory.cpp
    server/routes.cpp
    server/run_script_tool.cpp
    ${RUN_SCRIPT_BRIDGE}
    ${QUICKJS_SOURCES}
    third_party/sqlite3/sqlite3.c
)
target_include_directories(hermes_server PRIVATE third_party/httplib third_party/sqlite3 third_party/quickjs src)
target_link_libraries(hermes_server PRIVATE hermes_core winhttp ws2_32)

enable_testing()
add_executable(test_adapters tests/test_adapters.cpp)
target_link_libraries(test_adapters PRIVATE hermes_core)
add_test(NAME adapters COMMAND test_adapters)

add_executable(test_agent_events tests/test_agent_events.cpp)
target_link_libraries(test_agent_events PRIVATE hermes_core)
add_test(NAME agent_events COMMAND test_agent_events)

add_executable(test_new_tools tests/test_new_tools.cpp)
target_link_libraries(test_new_tools PRIVATE hermes_core)
add_test(NAME new_tools COMMAND test_new_tools)

add_executable(test_mcp tests/test_mcp.cpp server/mcp_client.cpp)
target_link_libraries(test_mcp PRIVATE hermes_core)
target_include_directories(test_mcp PRIVATE src server)
add_test(NAME mcp COMMAND test_mcp)

add_executable(test_session_store tests/test_session_store.cpp server/session_store.cpp third_party/sqlite3/sqlite3.c)
target_link_libraries(test_session_store PRIVATE hermes_core)
target_include_directories(test_session_store PRIVATE server third_party/sqlite3)
add_test(NAME session_store COMMAND test_session_store)

if(NOT WIN32)
    # QuickJS 沙盒单测:仅在能编译 QuickJS 的平台(Windows 用降级实现,QuickJS 逻辑不可测)
    add_executable(test_run_script
        tests/test_run_script.cpp server/run_script_tool.cpp ${RUN_SCRIPT_BRIDGE} ${QUICKJS_SOURCES})
    target_link_libraries(test_run_script PRIVATE hermes_core)
    target_include_directories(test_run_script PRIVATE server third_party/quickjs)
    add_test(NAME run_script COMMAND test_run_script)
endif()
