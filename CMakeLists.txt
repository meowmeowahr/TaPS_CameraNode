cmake_minimum_required(VERSION 3.28)
project(TaPS_CameraNode)

set(CMAKE_CXX_STANDARD 23)

include(FetchContent)

# Declare and fetch the repository
FetchContent_Declare(
    args
    GIT_REPOSITORY https://github.com/Taywee/args.git
    GIT_TAG        6.6.0
)
FetchContent_MakeAvailable(args)

find_package(OpenCV 5.0 REQUIRED)
find_package(spdlog REQUIRED)

add_executable(TaPS_CameraNode main.cpp
        cv_logger.cpp
        cv_logger.h
        camera_enumeration.cpp
        camera_enumeration.h
        fourcc.h
        runtime_args.h
        cv_cap.h
        video_queue.h)
target_link_libraries(TaPS_CameraNode PRIVATE ${OpenCV_LIBS} spdlog::spdlog taywee::args)
