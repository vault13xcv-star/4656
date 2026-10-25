include_directories(SYSTEM ${GTEST_INCLUDE_DIRS})
include_directories(SYSTEM ${GMOCK_INCLUDE_DIRS})

file(GLOB UNITTEST_SRC_FILES
    main.cpp

    esm/testfixedstring.cpp
    esm/testrefid.cpp
    esm/variant.cpp

    lua/testasync.cpp
    lua/testconfiguration.cpp
    lua/testinputactions.cpp
    lua/testl10n.cpp
    lua/testlua.cpp
    lua/testscriptscontainer.cpp
    lua/testserialization.cpp
    lua/teststorage.cpp
    lua/testuicontent.cpp
    lua/testutilpackage.cpp
    lua/testyaml.cpp

    misc/compression.cpp
    misc/progressreporter.cpp
    misc/testendianness.cpp
    misc/testmathutil.cpp
    misc/testresourcehelpers.cpp
    misc/teststringops.cpp

    nifloader/testbulletnifloader.cpp

    detournavigator/navigator.cpp
    detournavigator/settingsutils.cpp
    detournavigator/recastmeshbuilder.cpp
    detournavigator/gettilespositions.cpp
    detournavigator/recastmeshobject.cpp
    detournavigator/navmeshtilescache.cpp
    detournavigator/tilecachedrecastmeshmanager.cpp
    detournavigator/navmeshdb.cpp
    detournavigator/serialization.cpp
    detournavigator/asyncnavmeshupdater.cpp

    serialization/binaryreader.cpp
    serialization/binarywriter.cpp
    serialization/sizeaccumulator.cpp
    serialization/integration.cpp

    settings/parser.cpp
    settings/shadermanager.cpp
    settings/testvalues.cpp

    shader/parsedefines.cpp
    shader/parsefors.cpp
    shader/parselinks.cpp
    shader/shadermanager.cpp

    sqlite3/db.cpp
    sqlite3/request.cpp
    sqlite3/statement.cpp
    sqlite3/transaction.cpp

    esmloader/load.cpp
    esmloader/esmdata.cpp
    esmloader/record.cpp

    files/conversiontests.cpp
    files/hash.cpp

    toutf8/toutf8.cpp

    esm4/includes.cpp

    fx/lexer.cpp
    fx/technique.cpp

    esm3/readerscache.cpp
    esm3/testsaveload.cpp
    esm3/testesmwriter.cpp
    esm3/testinfoorder.cpp
    esm3/testcstringids.cpp

    nifosg/testnifloader.cpp

    esmterrain/testgridsampling.cpp

    resource/testobjectcache.cpp
    resource/testresourcesystem.cpp

    vfs/testpathutil.cpp

    sceneutil/osgacontroller.cpp
    sceneutil/testbloodlighting.cpp
    sceneutil/testbloodexterior.cpp

    bsa/testbsafile.cpp
    bsa/testcompressedbsafile.cpp

    nif/node.hpp
    nif/testphysics.cpp
)

source_group(apps\\components-tests FILES ${UNITTEST_SRC_FILES})

openmw_add_executable(components-tests ${UNITTEST_SRC_FILES})

target_sources(components-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/bloodsurface.cpp
    ${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/bloodsurfacevisitor.cpp)
target_compile_definitions(components-tests PRIVATE BT_USE_DOUBLE_PRECISION)
target_include_directories(components-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})

target_link_libraries(components-tests
    GTest::GTest
    GMock::GMock
    components
)

# The normal component suite remains usable without a display or EGL. Enable
# this separately to compile and draw the real ShaderManager blood/shadow
# programs against a surfaceless desktop OpenGL context on Linux.
option(BRUTAL_TEST_NATIVE_GL "Test native blood/shadow shader linkage with Linux EGL" OFF)
if(BRUTAL_TEST_NATIVE_GL AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
    add_executable(blood-native-gl-tests
        sceneutil/bloodshaders_gl.cpp
        ${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/brutalblood.cpp
        ${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/bloodcoverage.cpp
        ${CMAKE_SOURCE_DIR}/apps/openmw/mwrender/bloodsurface.cpp)
    target_compile_features(blood-native-gl-tests PRIVATE cxx_std_20)
    set_target_properties(blood-native-gl-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(blood-native-gl-tests PRIVATE BT_USE_DOUBLE_PRECISION
        OPENMW_PROJECT_SOURCE_DIR=u8"${PROJECT_SOURCE_DIR}")
    target_include_directories(blood-native-gl-tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_include_directories(blood-native-gl-tests SYSTEM PRIVATE ${BULLET_INCLUDE_DIRS})
    target_link_libraries(blood-native-gl-tests PRIVATE components OpenGL::GL OpenGL::EGL)
endif()

if (WIN32)
    target_sources(components-tests PRIVATE ${CMAKE_SOURCE_DIR}/files/windows/other-apps.manifest)
endif()

# Fix for not visible pthreads functions for linker with glibc 2.15
if (UNIX AND NOT APPLE)
    target_link_libraries(components-tests ${CMAKE_THREAD_LIBS_INIT})
endif()

if (BUILD_WITH_CODE_COVERAGE)
    target_compile_options(components-tests PRIVATE --coverage)
    target_link_libraries(components-tests gcov)
endif()

file(DOWNLOAD
    https://gitlab.com/OpenMW/example-suite/-/raw/8966dab24692555eec720c854fb0f73d108070cd/data/template.omwgame
    ${CMAKE_CURRENT_BINARY_DIR}/data/template.omwgame
    EXPECTED_HASH SHA512=6e38642bcf013c5f496a9cb0bf3ec7c9553b6e86b836e7844824c5a05f556c9391167214469b6318401684b702d7569896bf743c85aee4198612b3315ba778d6
)

target_compile_definitions(components-tests
    PRIVATE OPENMW_DATA_DIR=u8"${CMAKE_CURRENT_BINARY_DIR}/data"
            OPENMW_PROJECT_SOURCE_DIR=u8"${PROJECT_SOURCE_DIR}")

if (MSVC AND PRECOMPILE_HEADERS_WITH_MSVC)
    target_precompile_headers(components-tests PRIVATE
        <boost/program_options/options_description.hpp>

        <gtest/gtest.h>

        <sol/sol.hpp>

        <algorithm>
        <filesystem>
        <fstream>
        <functional>
        <memory>
        <string>
        <vector>
    )
endif()
