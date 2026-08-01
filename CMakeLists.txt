cmake_minimum_required(VERSION 3.28)
project(TaPS_CameraNode)

set(CMAKE_CXX_STANDARD 23)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FetchContent)

# Declare and fetch the repository
FetchContent_Declare(
        args
        GIT_REPOSITORY https://github.com/Taywee/args.git
        GIT_TAG 6.6.0
)
FetchContent_MakeAvailable(args)

FetchContent_Declare(
        json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

get_property(_targets DIRECTORY "${libhttpserver_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS "libhttpserver targets: ${_targets}")
find_package(OpenCV 5.0 REQUIRED)
find_package(spdlog REQUIRED)
find_package(LibHttpServer REQUIRED)

add_executable(TaPS_CameraNode main.cpp
        cv_logger.cpp
        cv_logger.h
        camera_enumeration.cpp
        camera_enumeration.h
        fourcc.h
        runtime_args.h
        cv_cap.h
        video_queue.h
        video_recorder.h)

target_link_libraries(TaPS_CameraNode PRIVATE ${OpenCV_LIBS} spdlog::spdlog taywee::args httpserver nlohmann_json::nlohmann_json)
target_include_directories(TaPS_CameraNode PRIVATE
        ${libhttpserver_SOURCE_DIR}/src
        ${libhttpserver_BINARY_DIR}/src
)