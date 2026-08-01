#include <chrono>
#include <csignal>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <spdlog/spdlog.h>

#include <args.hxx>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <vector>

#include "camera_enumeration.h"
#include "cv_cap.h"
#include "cv_logger.h"
#include "fourcc.h"
#include "runtime_args.h"
#include "video_queue.h"

using namespace cv;
using namespace std;

struct TimestampedFrame {
    cv::Mat frame;
    std::chrono::nanoseconds ptpTimestamp; // PTP/System clock timestamp
};
static VideoBuffer<TimestampedFrame>* g_frameBuffer = nullptr;
static std::thread* g_recorderThread = nullptr;

void recorder(VideoBuffer<TimestampedFrame>& buffer,
              const std::string& outputFile,
              int width, int height, double targetFps)
{
    spdlog::info("Start recorder thread targeting {}", outputFile);

    // int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    // VideoWriter writer(outputFile, cv::CAP_FFMPEG, fourcc, targetFps, cv::Size(width, height));
    std::ofstream output(outputFile, std::ios::binary);

    // if (!writer.isOpened()) {
    //     spdlog::error("Failed to open MKV VideoWriter for path: {}", outputFile);
    //     return;
    // }

    std::string csvPath = outputFile + ".timing.csv";
    std::ofstream ptpLog(csvPath);
    if (ptpLog.is_open()) {
        ptpLog << "frame_index,ptp_ns,delta_ms\n";
    } else {
        spdlog::warn("Could not open PTP sidecar log: {}", csvPath);
    }

    uint64_t frameIdx = 0;
    while (auto item = buffer.pop()) {

        if (!item.value().frame.empty()) {
            // writer.write(item.value().frame);
            output.write(
                reinterpret_cast<const char*>(item.value().frame.data),
                static_cast<std::streamsize>(item.value().frame.total() * item.value().frame.elemSize())
            );

            int64_t currentNs = item.value().ptpTimestamp.count();
            if (ptpLog.is_open()) {
                ptpLog << frameIdx << "," << currentNs << "\n";
            }

            if (buffer.pushClosed()) {
                spdlog::info("frames left before safe shutdown: {}", buffer.size());
            }
            frameIdx++;
        }
    }

    // writer.release();
    output.close();
    if (ptpLog.is_open()) {
        ptpLog.close();
    }

    spdlog::info("Recorder finished. Wrote {} frames to {} (and {})", frameIdx, outputFile, csvPath);
}

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
    args::ValueFlag widthFlag(parser, "width", "Target frame width", {'w', "width"}, flags.width);
    args::ValueFlag heightFlag(parser, "height", "Target frame height", {'h', "height"}, flags.height);
    args::ValueFlag fourccFlag(parser, "fourcc", "Target FourCC string", {'F', "fourcc"},
                               fourcc_to_string(flags.fourcc));

    args::ValueFlag rollingFpsFrameCountFlag(parser, "frames", "Frame count for fps averaging", {"rolling-fps-frames"},
                                             flags.rollingFpsFrameCount);
    args::ValueFlag fpsReportingIntervalFlag(parser, "frames", "How often to report FPS, 0 to disable",
                                             {"fps-interval"}, flags.fpsReportingInterval);

    // New flags for buffer
    args::ValueFlag bufferMaxSizeFlag(parser, "buffer-max-size", "Maximum number of frames to buffer (0 for unlimited)",
                                      {"buffer-max-size"}, flags.bufferMaxSize);

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
    g_frameBuffer = &frameBuffer;
    std::thread recorderThread(recorder, std::ref(frameBuffer), "output/rec.mkv", flags.width, flags.height, flags.fps);
    g_recorderThread = &recorderThread;

    std::signal(SIGINT, [](int sig) {
        if (g_frameBuffer) {
            g_frameBuffer->shutdown();
        }
        g_recorderThread->join();
        std::exit(sig);
    });

    vector<double> delta_times;
    delta_times.reserve(flags.rollingFpsFrameCount);

    unsigned long long frame_count = 0;

    auto start_time = chrono::high_resolution_clock::now();

    for (;;) {
        cap.read(frame);

        if (frame.empty()) {
            spdlog::error("blank frame grabbed");
            break;
        }

        if (frame.rows != flags.height || frame.cols != flags.width) {
            spdlog::critical("frame size from camera {}x{} != expected {}x{}", frame.cols, frame.rows, flags.width,
                             flags.height);
            return 1;
        }

        auto end_time = chrono::high_resolution_clock::now();

        chrono::duration<double, milli> elapsed =
                end_time - start_time;

        double delta_ms = elapsed.count();

        start_time = end_time;

        delta_times.push_back(delta_ms);
        frame_count++;

        if (!frameBuffer.tryPush(TimestampedFrame{frame, std::chrono::system_clock::now().time_since_epoch()})) {
            spdlog::warn("buffer full, dropping frame #{}", frame_count);
        }

        if (frame_count >= flags.rollingFpsFrameCount) {
            double sum_dt = 0.0;

            for (const double dt: delta_times)
                sum_dt += dt;

            const double mean_dt =
                    sum_dt / flags.rollingFpsFrameCount;

            double rate = 1000.0 / mean_dt;

            if (frame_count % flags.fpsReportingInterval == 0) {
                spdlog::info(
                    "rolling avg fps of last {} frames: {:.2f}fps",
                    flags.rollingFpsFrameCount, rate);
                spdlog::info("buffer health: {}/{}", frameBuffer.size(), flags.bufferMaxSize);
            }

            delta_times.erase(delta_times.begin());
        }
    }

    return 0;
}
