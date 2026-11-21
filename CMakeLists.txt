cmake_minimum_required(VERSION 3.16)
project(agentlink-qt-client VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Qt6：Core（JSON / 事件循环）+ Network（WebSocket / HTTP）
# Widgets 是**可选**的 —— 只有 BUILD_GUI=ON 时才找它。
# 这样协议核心（agentlink_core）和它的测试可以在没有 X11 dev 库的环境里编译运行，
# 只有要编出界面才需要 Widgets。
option(BUILD_GUI "Build the Qt Widgets GUI" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets Test)

if(BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
endif()

# ---------------------------------------------------------------------------
# agentlink_core：协议核心，**不依赖 GUI**
# ---------------------------------------------------------------------------
# 单独成库是为了能不起窗口就测协议层（见 tests/）。
add_library(agentlink_core STATIC
  src/core/frames.cpp
  src/core/client.cpp
  src/core/auth_api.cpp
  src/core/settings.cpp
)
target_include_directories(agentlink_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(agentlink_core PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets)
target_compile_options(agentlink_core PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------------
# agentlink-qt：可执行程序（仅在 BUILD_GUI=ON 时构建；无 X11 dev 库时只编 core + tests）
# ---------------------------------------------------------------------------
if(BUILD_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  add_executable(agentlink-qt
    src/main.cpp
    src/gui/main_window.cpp
    src/gui/onboarding.cpp
    src/gui/chat_window.cpp
    src/gui/conversation_list.cpp
  )
  target_link_libraries(agentlink-qt PRIVATE agentlink_core Qt6::Widgets)
  target_compile_options(agentlink-qt PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()

# 帧编解码：不需要网络，纯函数
add_executable(test-frames tests/test_frames.cpp)
target_link_libraries(test-frames PRIVATE agentlink_core Qt6::Test)
add_test(NAME frames COMMAND test-frames)

# 服务器地址解析：决定"用户填的地址能不能用"
add_executable(test-address tests/test_address.cpp)
target_link_libraries(test-address PRIVATE agentlink_core Qt6::Test)
add_test(NAME address COMMAND test-address)

# 设置存储：多服务器的读写
add_executable(test-settings tests/test_settings.cpp)
target_link_libraries(test-settings PRIVATE agentlink_core Qt6::Test)
add_test(NAME settings COMMAND test-settings)

# 端到端：真连服务端（需要 AGENTLINK_TEST_URL 环境变量）
add_executable(test-e2e tests/test_e2e.cpp)
target_link_libraries(test-e2e PRIVATE agentlink_core Qt6::Test)
add_test(NAME e2e COMMAND test-e2e)
set_tests_properties(e2e PROPERTIES SKIP_RETURN_CODE 77)
