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

add_executable(item-physics-tests items.cpp tests/items.cpp)
target_compile_features(item-physics-tests PRIVATE cxx_std_20)
target_compile_definitions(item-physics-tests PRIVATE BT_USE_DOUBLE_PRECISION)
target_include_directories(item-physics-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})
target_link_libraries(item-physics-tests PRIVATE ${BULLET_LIBRARIES})
add_test(NAME item-physics COMMAND item-physics-tests)

option(RAGDOLL_TEST_RENDERING "Test physical poses and lethal-hit attribution with OpenSceneGraph" OFF)
option(RAGDOLL_TEST_GL "Run actual surfaceless EGL fire/smoke regression tests (Linux)" OFF)
if(RAGDOLL_TEST_RENDERING)
    find_package(OpenGL REQUIRED)
    find_package(OpenSceneGraph 3.6.5 REQUIRED COMPONENTS osg osgUtil)
    add_executable(ragdoll-render-tests
        tests/render_pose.cpp
        simulation.cpp
        items.cpp
        ../../apps/openmw/mwrender/ragdoll.cpp
        ../../apps/openmw/mwrender/ragdollappearance.cpp
        ../../apps/openmw/mwrender/brutalgore.cpp
        ../../apps/openmw/mwrender/brutalblood.cpp
        ../sceneutil/skeleton.cpp
        ../sceneutil/riggeometry.cpp
        ../sceneutil/morphgeometry.cpp
        ../debug/debuglog.cpp
        ../files/conversion.cpp)
    target_compile_features(ragdoll-render-tests PRIVATE cxx_std_20)
    target_compile_definitions(ragdoll-render-tests PRIVATE BT_USE_DOUBLE_PRECISION)
    target_include_directories(ragdoll-render-tests PRIVATE ../..)
    target_include_directories(ragdoll-render-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS} ${OPENSCENEGRAPH_INCLUDE_DIRS})
    target_link_libraries(ragdoll-render-tests PRIVATE ${BULLET_LIBRARIES} ${OPENSCENEGRAPH_LIBRARIES} OpenGL::GL)
    add_test(NAME ragdoll-rendering COMMAND ragdoll-render-tests)

    add_executable(ragdoll-impact tests/impact.cpp)
    target_compile_features(ragdoll-impact PRIVATE cxx_std_20)
    target_include_directories(ragdoll-impact PRIVATE ../..)
    target_include_directories(ragdoll-impact SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS} ${BULLET_INCLUDE_DIRS})
    target_compile_definitions(ragdoll-impact PRIVATE BT_USE_DOUBLE_PRECISION)
    target_link_libraries(ragdoll-impact PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
    add_test(NAME ragdoll-impact COMMAND ragdoll-impact)
    add_executable(physical-combat-tests tests/physicalcombat.cpp)
    target_compile_features(physical-combat-tests PRIVATE cxx_std_20)
    target_include_directories(physical-combat-tests PRIVATE ../..)
    target_include_directories(physical-combat-tests SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})
    target_link_libraries(physical-combat-tests PRIVATE ${OPENSCENEGRAPH_LIBRARIES})
    add_test(NAME physical-combat COMMAND physical-combat-tests)
    if(RAGDOLL_TEST_GL)
        find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
        find_package(OpenSceneGraph 3.6.5 REQUIRED COMPONENTS osg osgUtil osgViewer osgGA)
        add_executable(ragdoll-gl-tests tests/appearance_gl.cpp ../../apps/openmw/mwrender/ragdollappearance.cpp
            ../../apps/openmw/mwrender/brutalblood.cpp)
        target_compile_features(ragdoll-gl-tests PRIVATE cxx_std_20)
        target_include_directories(ragdoll-gl-tests PRIVATE ../..)
        target_include_directories(ragdoll-gl-tests SYSTEM PRIVATE ${OPENSCENEGRAPH_INCLUDE_DIRS})
        target_include_directories(ragdoll-gl-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})
        target_compile_definitions(ragdoll-gl-tests PRIVATE BT_USE_DOUBLE_PRECISION)
        target_link_libraries(ragdoll-gl-tests PRIVATE ${OPENSCENEGRAPH_LIBRARIES} ${BULLET_LIBRARIES} OpenGL::GL OpenGL::EGL)
        add_test(NAME ragdoll-gl-normal COMMAND ragdoll-gl-tests)
        add_test(NAME ragdoll-gl-reversed COMMAND ragdoll-gl-tests reversed)
        add_test(NAME ragdoll-gl-poison COMMAND ragdoll-gl-tests normal poison)
        add_test(NAME ragdoll-gl-poison-reversed COMMAND ragdoll-gl-tests reversed poison)
        add_test(NAME ragdoll-gl-shock COMMAND ragdoll-gl-tests normal shock)
        add_test(NAME ragdoll-gl-shock-reversed COMMAND ragdoll-gl-tests reversed shock)
        add_test(NAME ragdoll-gl-wounds COMMAND ragdoll-gl-tests normal wounds)
        add_test(NAME ragdoll-gl-wounds-reversed COMMAND ragdoll-gl-tests reversed wounds)
        add_test(NAME brutal-gl-blood COMMAND ragdoll-gl-tests normal blood)
        add_test(NAME brutal-gl-blood-reversed COMMAND ragdoll-gl-tests reversed blood)
    endif()
endif()
