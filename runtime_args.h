//
// Created by kevin on 7/31/26.
//

#ifndef TAPS_CAMERANODE_RUNTIME_ARGS_H
#define TAPS_CAMERANODE_RUNTIME_ARGS_H

enum class EncoderType {
    JPEG,
    RAW
};

struct RuntimeArgs {
    int cameraId = 0;
    unsigned short fps = 60;
    unsigned long width = 640;
    unsigned long height = 480;
    unsigned long fourcc = 0x47504a4d; // MJPG
    unsigned short rollingFpsFrameCount = 100;
    unsigned short fpsReportingInterval = 100;
    unsigned long bufferMaxSize = 0; // 0 means unlimited
    EncoderType encoderType = EncoderType::JPEG; // default to JPEG
    std::string encoderArgs = ""; // encoder-specific arguments like "quality:90" or "order:rgb"
};

#endif //TAPS_CAMERANODE_RUNTIME_ARGS_H