cmake_minimum_required(VERSION 3.8)
project(icp_odometry)

find_package(ament_cmake REQUIRED)
find_package(Boost COMPONENTS thread filesystem system program_options date_time chrono timer serialization REQUIRED)
find_package(common REQUIRED)
find_package(geom REQUIRED)
find_package(libpointmatcher REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(truck_msgs REQUIRED)
find_package(rclcpp REQUIRED)

add_executable(
  ${PROJECT_NAME}_node
  src/main.cpp
  src/icp_odometry_node.cpp
  src/conversion.cpp
)

ament_target_dependencies(
    ${PROJECT_NAME}_node
    common
    geom
    libpointmatcher
    rclcpp
    sensor_msgs
    Boost
    truck_msgs)

target_include_directories(${PROJECT_NAME}_node PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>"
  "$<INSTALL_INTERFACE:include/${PROJECT_NAME}>")

install(
    TARGETS ${PROJECT_NAME}_node
    DESTINATION lib/${PROJECT_NAME})

install(
    DIRECTORY launch config
    DESTINATION share/${PROJECT_NAME})

ament_package()
