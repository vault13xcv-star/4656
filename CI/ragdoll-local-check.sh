#!/usr/bin/env bash
# Compile every modified engine translation unit, then execute the standalone
# Bullet and OpenSceneGraph regression tests. Requires the normal OpenMW Linux
# development dependencies, CMake, Ninja and a C++20 compiler.
#
# Usage: CI/ragdoll-local-check.sh [build-directory] [additional CMake arguments...]
# CMAKE_PREFIX_PATH, PKG_CONFIG_LIBDIR and PKG_CONFIG_SYSROOT_DIR can select an
# isolated dependency prefix. This does not build or playtest the game runtime.
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-"$source_dir/build-ragdoll-check"}
if (( $# > 0 )); then
    shift
fi

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_OPENMW=ON \
    -DBUILD_LAUNCHER=OFF -DBUILD_WIZARD=OFF -DBUILD_OPENCS=OFF \
    -DBUILD_MWINIIMPORTER=OFF -DBUILD_ESSIMPORTER=OFF \
    -DBUILD_BSATOOL=OFF -DBUILD_ESMTOOL=OFF -DBUILD_NIFTEST=OFF \
    -DBUILD_NAVMESHTOOL=OFF -DBUILD_BULLETOBJECTTOOL=OFF \
    -DBUILD_COMPONENTS_TESTS=OFF -DBUILD_OPENMW_TESTS=OFF "$@"

cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target \
    components/CMakeFiles/components.dir/ragdoll/simulation.cpp.o \
    components/CMakeFiles/components.dir/ragdoll/items.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwrender/ragdoll.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwrender/ragdollappearance.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwrender/animation.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwrender/npcanimation.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwclass/npc.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwclass/creature.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/combat.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/creaturestats.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/activespells.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/spellcasting.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwmechanics/spelleffects.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwlua/luamanagerimp.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwlua/types/actor.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwworld/projectilemanager.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwphysics/mtphysics.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwphysics/physicssystem.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwphysics/itemphysics.cpp.o \
    apps/openmw/CMakeFiles/openmw-lib.dir/mwworld/worldimp.cpp.o

test_dir="$build_dir/ragdoll-tests"
cmake -S "$source_dir/components/ragdoll" -B "$test_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DRAGDOLL_TEST_RENDERING=ON "$@"
cmake --build "$test_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir "$test_dir" --output-on-failure
