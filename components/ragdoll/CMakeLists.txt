cmake_minimum_required(VERSION 3.16)
project(OpenMWRagdollPhysicsTests LANGUAGES CXX)

enable_testing()

# This small standalone build exercises the real Bullet solver without game
# assets, OpenSceneGraph, or the rest of OpenMW. It uses the same double precision
# ABI required by the engine, including on the Windows build runner.
if(NOT BULLET_INCLUDE_DIRS OR NOT BULLET_LIBRARIES)
    find_package(Bullet CONFIGS BulletConfig-float64.cmake QUIET)
    if(BULLET_FOUND)
        set(BULLET_INCLUDE_DIRS "${BULLET_ROOT_DIR}/${BULLET_INCLUDE_DIRS}")
    else()
        find_package(Bullet REQUIRED)
    endif()
endif()
string(REGEX MATCHALL "((optimized|debug);)?[^;]*(BulletDynamics|BulletCollision|LinearMath)[^;]*"
    BULLET_LIBRARIES "${BULLET_LIBRARIES}")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/CheckBulletPrecision.cmake")
if(NOT HAS_DOUBLE_PRECISION_BULLET)
    message(FATAL_ERROR "Ragdoll tests require double precision Bullet libraries")
endif()

add_executable(ragdoll-tests simulation.cpp tests/simulation.cpp)
target_compile_features(ragdoll-tests PRIVATE cxx_std_17)
target_compile_definitions(ragdoll-tests PRIVATE BT_USE_DOUBLE_PRECISION)
target_include_directories(ragdoll-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})
target_link_libraries(ragdoll-tests PRIVATE ${BULLET_LIBRARIES})
if(MSVC)
    target_compile_options(ragdoll-tests PRIVATE /W4)
else()
    target_compile_options(ragdoll-tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME ragdoll-physics COMMAND ragdoll-tests)

option(RAGDOLL_TEST_RENDERING "Test physical poses on an OpenSceneGraph skeleton" OFF)
if(RAGDOLL_TEST_RENDERING)
    find_package(OpenSceneGraph 3.6.5 REQUIRED COMPONENTS osg osgUtil)
    add_executable(ragdoll-render-tests
        tests/render_pose.cpp
        simulation.cpp
        ../../apps/openmw/mwrender/ragdoll.cpp
        ../sceneutil/skeleton.cpp
        ../debug/debuglog.cpp
        ../files/conversion.cpp)
    target_compile_features(ragdoll-render-tests PRIVATE cxx_std_20)
    target_compile_definitions(ragdoll-render-tests PRIVATE BT_USE_DOUBLE_PRECISION)
    target_include_directories(ragdoll-render-tests PRIVATE ../..)
    target_include_directories(ragdoll-render-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS} ${OPENSCENEGRAPH_INCLUDE_DIRS})
    target_link_libraries(ragdoll-render-tests PRIVATE ${BULLET_LIBRARIES} ${OPENSCENEGRAPH_LIBRARIES})
    add_test(NAME ragdoll-rendering COMMAND ragdoll-render-tests)
endif()
