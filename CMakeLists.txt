cmake_minimum_required(VERSION 3.8)
project(rmtc_bot CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -O2)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)   # sudo apt install nlohmann-json3-dev
find_package(CURL REQUIRED)            # sudo apt install libcurl4-openssl-dev

add_executable(contestant
  src/main.cpp
  src/llm.cpp
)

target_include_directories(contestant PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
ament_target_dependencies(contestant rclcpp std_msgs)
target_link_libraries(contestant nlohmann_json::nlohmann_json CURL::libcurl)

# DESTINATION 必须是 lib/${PROJECT_NAME}，ros2 run 只认这里
install(TARGETS contestant DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY launch config scripts DESTINATION share/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
