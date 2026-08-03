FROM mcr.microsoft.com/devcontainers/cpp:3-debian13

ARG REINSTALL_CMAKE_VERSION_FROM_SOURCE="none"

# Optionally install the cmake for vcpkg
COPY ./reinstall-cmake.sh /tmp/

RUN if [ "${REINSTALL_CMAKE_VERSION_FROM_SOURCE}" != "none" ]; then \
        chmod +x /tmp/reinstall-cmake.sh && /tmp/reinstall-cmake.sh ${REINSTALL_CMAKE_VERSION_FROM_SOURCE}; \
    fi \
    && rm -f /tmp/reinstall-cmake.sh

# [Optional] Uncomment this section to install additional vcpkg ports.
# RUN su vscode -c "${VCPKG_ROOT}/vcpkg install <your-port-name-here>"

# [Optional] Uncomment this section to install additional packages.
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
 && apt-get -y install --no-install-recommends build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config \
    ca-certificates \
    wget \
    unzip \
    && rm -rf /var/lib/apt/lists/*

# OpenCV dependencies
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
 && apt-get -y install --no-install-recommends \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libv4l-dev \
    libxvidcore-dev \
    libx264-dev \
    libjpeg-dev \
    libpng-dev \
    libtiff-dev \
    libopenexr-dev \
    libtbb-dev \
    libeigen3-dev \
    && rm -rf /var/lib/apt/lists/*

ARG OPENCV_VERSION=5.0.0

RUN wget -q \
        https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz \
        -O /tmp/opencv.tar.gz \
    && mkdir -p /tmp/opencv \
    && tar -xzf /tmp/opencv.tar.gz --strip-components=1 -C /tmp/opencv \
    && cmake -S /tmp/opencv -B /tmp/opencv-build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_TESTS=OFF \
        -DBUILD_PERF_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_opencv_apps=OFF \
        -DWITH_TBB=ON \
        -DWITH_V4L=ON \
        -DWITH_FFMPEG=ON \
    && cmake --build /tmp/opencv-build \
    && cmake --install /tmp/opencv-build \
    && rm -rf /tmp/opencv /tmp/opencv-build /tmp/opencv.tar.gz

RUN ldconfig