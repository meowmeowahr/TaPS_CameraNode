#include <chrono>
#include <csignal>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <args.hxx>
#include <deque>
#include <fstream>
#include <mutex>
#include <vector>
#include <stdexcept>

#include "camera_enumeration.h"
#include "cv_cap.h"
#include "cv_logger.h"
#include "fourcc.h"
#include "runtime_args.h"
#include "video_queue.h"
#include "video_recorder.h"
#include "http_server.h"

static VideoBuffer<TimestampedFrame> *g_frameBuffer = nullptr;
static VideoBuffer<TimestampedFrame> *g_frameBufferStream = nullptr;

using namespace cv;
using namespace std;

int main(const int argc, char *argv[]) {
    // ReSharper disable once CppUseStructuredBinding
    auto flags = RuntimeArgs{};
    args::ArgumentParser parser("Camera Node for FRC Tracking and Playback System");
    args::HelpFlag help(parser, "help", "Display this help menu", {'?', "help"});

    args::Flag enumerateOnly(parser, "enumerate", "Enumerate supported V4L2 camera modes, and exit",
                             {"enum", "enumerate"});
    args::Flag verboseFlag(parser, "verbose", "Enable verbose logging (debug level)", {'v', "verbose"});
    args::Flag traceFlag(parser, "trace", "Enable trace logging (highest detail)", {'t', "trace"});

    args::ValueFlag cameraIdFlag(parser, "camera", "Camera ID as in /dev/videoX", {'c', "camera"}, flags.cameraId);
    args::ValueFlag fpsFlag(parser, "fps", "Target frame rate", {'f', "fps"}, flags.fps);
    args::ValueFlag widthFlag(parser, "px", "Target frame width", {'w', "width"}, flags.width);
    args::ValueFlag heightFlag(parser, "px", "Target frame height", {'h', "height"}, flags.height);
    args::ValueFlag fourccFlag(parser, "fourcc", "Target FourCC string", {'F', "fourcc"},
                               fourcc_to_string(flags.fourcc));

    args::ValueFlag rollingFpsFrameCountFlag(parser, "frames", "Frame count for fps averaging", {"rolling-fps-frames"},
                                             flags.rollingFpsFrameCount);
    args::ValueFlag fpsReportingIntervalFlag(parser, "frames", "How often to report FPS, 0 to disable",
                                             {"fps-interval"}, flags.fpsReportingInterval);

    args::ValueFlag bufferMaxSizeFlag(parser, "buffer-max-size", "Maximum number of frames to buffer (0 for unlimited)",
                                      {"buffer-max-size"}, flags.bufferMaxSize);

    args::ValueFlag encoderFlag(parser, "encoder", "Encoder type: jpeg or raw", {'e', "encoder"}, string("jpeg"));

    args::ValueFlag encoderArgsFlag(parser, "encoder-args",
                                    "Encoder arguments (e.g., quality:90 for jpeg, order:rgb/bgr/gray/bgr565/bgr555 for raw)",
                                    {'a', "encoder-args"}, flags.encoderArgs);

    args::ValueFlag encoderThreadsFlag(parser, "threads", "Number of threads for the encoder", {'j', "encoder-threads"},
                                       flags.encoderThreads);

    args::ValueFlag outputDirFlag(parser, "dir", "Output directory, defaults to output/", {'o', "output"},
                                  flags.outputDir);

    args::ValueFlag httpPortFlag(parser, "http-port", "HTTP server port", {'p', "http-port"}, flags.httpPort);

    args::ValueFlag streamJpegQualityFlag(parser, "quality", "MJPEG streaming quality", {'q', "stream-quality"},
                                          flags.jpegStreamQuality);
    args::ValueFlag streamJpegWidthFlag(parser, "px", "MJPEG streaming width", {'W', "stream-width"},
                                        flags.jpegStreamWidth);
    args::ValueFlag streamJpegHeightFlag(parser, "px", "MJPEG streaming height", {'H', "stream-height"},
                                         flags.jpegStreamHeight);
    args::ValueFlag streamJpegFpsFlag(parser, "fps", "MJPEG streaming FPS", {"stream-fps"}, flags.jpegStreamFps);

    args::CompletionFlag completion(parser, {"complete"});

    try {
        parser.ParseCLI(argc, argv);
    } catch (const args::Completion &e) {
        std::cout << e.what();
        return 0;
    } catch (const args::Help &) {
        std::cout << parser;
        return 0;
    } catch (const args::ParseError &e) {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    if (traceFlag) {
        spdlog::set_level(spdlog::level::trace);
    } else if (verboseFlag) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    flags.cameraId = args::get(cameraIdFlag);
    flags.fps = args::get(fpsFlag);
    flags.width = args::get(widthFlag);
    flags.height = args::get(heightFlag);
    flags.fourcc = string_to_fourcc(args::get(fourccFlag));
    flags.rollingFpsFrameCount = args::get(rollingFpsFrameCountFlag);
    flags.fpsReportingInterval = args::get(fpsReportingIntervalFlag);
    flags.bufferMaxSize = args::get(bufferMaxSizeFlag);

    // Handle encoder flag
    if (std::string encoderStr = args::get(encoderFlag); encoderStr == "jpeg") {
        flags.encoderType = EncoderType::JPEG;
    } else if (encoderStr == "raw") {
        flags.encoderType = EncoderType::RAW;
    } else {
        throw std::invalid_argument("Encoder must be 'jpeg' or 'raw'");
    }

    flags.encoderArgs = args::get(encoderArgsFlag);
    flags.encoderThreads = args::get(encoderThreadsFlag);
    flags.outputDir = args::get(outputDirFlag);

    flags.httpPort = args::get(httpPortFlag);
    flags.jpegStreamQuality = args::get(streamJpegQualityFlag);
    flags.jpegStreamWidth = args::get(streamJpegWidthFlag);
    flags.jpegStreamHeight = args::get(streamJpegHeightFlag);
    flags.jpegStreamFps = args::get(streamJpegFpsFlag);

    if (enumerateOnly) {
        enumerate_camera_modes(flags.cameraId);
        return 0;
    }

    spdlog_opencv_init();
    spdlog::info("Starting TaPS Camera Node");

    Mat frame;
    VideoCapture cap;
    cv_cap_setup(&cap, flags);

    auto frameBuffer = VideoBuffer<TimestampedFrame>(flags.bufferMaxSize);
    auto frameBufferStream = VideoBuffer<TimestampedFrame>(flags.bufferMaxSize);
    g_frameBuffer = &frameBuffer;
    g_frameBufferStream = &frameBufferStream;

    // Start video recorder
    VideoRecordThread::begin(&frameBuffer, flags.outputDir, flags);

    // Start HTTP server
    HttpServer::begin(&frameBufferStream, flags);

    std::signal(SIGINT, [](const int sig) {
        std::cout << "\n";
        spdlog::warn("SIGINT received");
        g_frameBuffer->shutdown();
        g_frameBufferStream->shutdown();
        VideoRecordThread::shutdown();
        HttpServer::stop();
        std::exit(sig);
    });

    vector<double> delta_times;
    delta_times.reserve(flags.rollingFpsFrameCount);

    unsigned long long frame_count = 0;

    auto start_time = chrono::high_resolution_clock::now();

    if (cap.get(CAP_PROP_FRAME_HEIGHT) != flags.height || cap.get(CAP_PROP_FRAME_WIDTH) != flags.width) {
        spdlog::critical("frame size from camera {}x{} != expected {}x{}", frame.cols, frame.rows, flags.width,
                         flags.height);
        VideoRecordThread::shutdown();
        HttpServer::stop();
        std::exit(1);
    }

    for (;;) {
        cap.read(frame);

        if (frame.empty()) {
            spdlog::error("blank frame grabbed");
            break;
        }

        auto end_time = chrono::high_resolution_clock::now();

        chrono::duration<double, milli> elapsed =
                end_time - start_time;

        double delta_ms = elapsed.count();

        start_time = end_time;

        bool streamEnabled = (flags.jpegStreamFps > 0);
        std::chrono::steady_clock::time_point lastStreamPush;
        std::chrono::microseconds streamInterval;
        if (streamEnabled) {
            lastStreamPush = std::chrono::steady_clock::now();
            streamInterval = std::chrono::microseconds(1'000'000 / flags.jpegStreamFps);
        }

        delta_times.push_back(delta_ms);
        frame_count++;

        if (VideoRecordThread::getState() != VideoRecordThread::RecorderState::Saving) {
            if (!frameBuffer.tryPush(TimestampedFrame{
                .frame = frame, .ptpTimestamp = std::chrono::system_clock::now().time_since_epoch()
            })) {
                spdlog::warn("record buffer full, dropping frame #{}", frame_count);
            }
        }

        if (streamEnabled) {
            if (auto now = std::chrono::steady_clock::now(); now - lastStreamPush >= streamInterval) {
                if (!frameBufferStream.tryPush(TimestampedFrame{
                    .frame = frame, .ptpTimestamp = std::chrono::system_clock::now().time_since_epoch()
                })) {
                    spdlog::warn("stream buffer full, dropping frame #{}", frame_count);
                }
                lastStreamPush = now;
            }
        }

        if (frame_count >= flags.rollingFpsFrameCount) {
            double sum_dt = std::accumulate(delta_times.begin(), delta_times.end(), 0.0);
            const double mean_dt = sum_dt / flags.rollingFpsFrameCount;
            double rate = 1000.0 / mean_dt;

            if (frame_count % flags.fpsReportingInterval == 0) {
                spdlog::debug(
                    "rolling avg fps of last {} frames: {:.2f}fps",
                    flags.rollingFpsFrameCount, rate);
                spdlog::debug("buffer usage: {}/{}", frameBuffer.size(), flags.bufferMaxSize);
            }

            delta_times.erase(delta_times.begin());
        }
    }
    // Cleanup
    VideoRecordThread::shutdown();
    HttpServer::stop();
    return 0;
}
