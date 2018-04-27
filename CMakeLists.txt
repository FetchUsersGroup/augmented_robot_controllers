cmake_minimum_required(VERSION 2.8.3)
project(augmented_robot_controllers)

add_compile_options(-std=c++11)
if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter")
endif()
set( CMAKE_EXPORT_COMPILE_COMMANDS ON )
find_package(orocos_kdl REQUIRED)
find_package(Boost REQUIRED system)
find_package(catkin REQUIRED
  COMPONENTS
    moveit_core
    moveit_ros_planning
    actionlib
    actionlib_msgs
    angles
    control_msgs
    geometry_msgs
    kdl_parser
    nav_msgs
    pluginlib
    robot_controllers_interface
    robot_controllers
    roscpp
    sensor_msgs
    tf
    tf_conversions
    trajectory_msgs
    urdf
)

catkin_package(
  INCLUDE_DIRS
    include
  CATKIN_DEPENDS
    moveit_core
    actionlib
    actionlib_msgs
    control_msgs
    geometry_msgs
    kdl_parser
    nav_msgs
    pluginlib
    robot_controllers_interface
    robot_controllers
    roscpp
    sensor_msgs
    tf
    tf_conversions
    trajectory_msgs
    urdf
  DEPENDS
    Boost
    orocos_kdl
  LIBRARIES
    robot_controllers
)

include_directories(
  include
  ${Boost_INCLUDE_DIRS}
  ${catkin_INCLUDE_DIRS}
  ${orocos_kdl_INCLUDE_DIRS}
)

# this is a hack, will eventually be unneeded once orocos-kdl is fixed
link_directories(${orocos_kdl_LIBRARY_DIRS})

add_library(augmented_robot_controllers
  src/cartesian_twist_avoid.cpp
)
target_link_libraries(augmented_robot_controllers
  ${catkin_LIBRARIES}
  ${orocos_kdl_LIBRARIES}
)

install(DIRECTORY include/ DESTINATION include)

install(FILES augmented_robot_controllers.xml
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}
)

install(
  TARGETS augmented_robot_controllers
  ARCHIVE DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
  LIBRARY DESTINATION ${CATKIN_PACKAGE_LIB_DESTINATION}
)
