# Nordstjernen — Android JNI bridge build.
#
# The full browser engine (engine + lexbor + quickjs + wuffs) is cross-compiled
# separately via meson into a per-ABI libnordstjernen.so dropped in
# app/src/main/jniLibs/<abi>/ (see android/scripts/build-deps.sh). This file
# only builds the thin JNI bridge.
#
# If that prebuilt engine .so is present for the ABI being built, the real
# bridge (nd_jni.c) is compiled and linked against it. Otherwise a stub bridge
# is built so the APK still assembles and runs with the engine reported
# unavailable — useful for UI work and CI before the dep stack is cross-built.

cmake_minimum_required(VERSION 3.22.1)
project(nordstjernen_jni C)

set(CMAKE_C_STANDARD 11)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(ENGINE_HEADER_DIR "${REPO_ROOT}/src")
set(PREBUILT_ENGINE "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libnordstjernen.so")

if(EXISTS "${PREBUILT_ENGINE}")
    message(STATUS "nordstjernen: using prebuilt engine ${PREBUILT_ENGINE}")
    add_library(nordstjernen SHARED IMPORTED)
    set_target_properties(nordstjernen PROPERTIES IMPORTED_LOCATION "${PREBUILT_ENGINE}")

    add_library(nordstjernen_jni SHARED nd_jni.c)
    target_include_directories(nordstjernen_jni PRIVATE "${ENGINE_HEADER_DIR}")
    target_link_libraries(nordstjernen_jni nordstjernen jnigraphics android log)
else()
    message(WARNING "nordstjernen: prebuilt engine not found for ${ANDROID_ABI}; building stub bridge")
    add_library(nordstjernen_jni SHARED nd_jni_stub.c)
    target_link_libraries(nordstjernen_jni log)
endif()
