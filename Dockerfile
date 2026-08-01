FROM debian:13

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    gdb \
    git \
    pkg-config \
    ca-certificates \
    wget \
    curl \
    ninja-build \
    libopencv-dev \
    libspdlog-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace