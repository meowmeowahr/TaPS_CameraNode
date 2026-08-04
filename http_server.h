//
// Created by kevin on 7/31/26.
//
#ifndef TAPS_CAMERANODE_HTTP_SERVER_H
#define TAPS_CAMERANODE_HTTP_SERVER_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/statvfs.h>

#include <httpserver.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "runtime_args.h"
#include "video_recorder.h"   // TimestampedFrame + VideoBuffer
#include "video_queue.h"

using namespace httpserver;
using json = nlohmann::json;

class HttpServer {
public:
    static void begin(VideoBuffer<TimestampedFrame> *frameBuffer, const RuntimeArgs &flags) {
        s_dataPath = flags.outputDir.empty() ? "/" : flags.outputDir;
        s_frameBuffer = frameBuffer;

        s_previewWidth = flags.jpegStreamWidth;
        s_previewHeight = flags.jpegStreamHeight;
        s_previewFps = flags.jpegStreamFps;
        s_jpegQuality = flags.jpegStreamQuality;

        s_ws = std::make_unique<webserver>(
            create_webserver(flags.httpPort)
            .log_access(custom_access_log)
            .log_error(custom_error_log)
            .max_threads(8)); // enough for a few concurrent streams

        s_ws->register_path("/status", std::make_unique<StatusResource>());
        s_ws->register_path("/recording", std::make_unique<RecordingResource>());
        s_ws->register_path("/disk", std::make_unique<DiskResource>());
        s_ws->register_path("/stream", std::make_unique<StreamResource>());
        s_ws->register_path("/", std::make_unique<IndexResource>());

        // Start the frame → JPEG pump
        s_running = true;
        s_encodeThread = std::thread(encodeLoop);

        s_ws->start(false); // non-blocking
        spdlog::info("HTTP server started on port {} (MJPEG at /stream)", flags.httpPort);
    }

    static void stop() {
        s_running = false;
        if (s_encodeThread.joinable())
            s_encodeThread.join();

        if (s_ws) {
            s_ws->stop();
            s_ws.reset();
        }
    }

private:
    static inline std::mutex s_jpegMutex;
    static inline std::vector<uint8_t> s_latestJpeg;
    static inline std::atomic<uint64_t> s_jpegSequence{0};

    static inline VideoBuffer<TimestampedFrame> *s_frameBuffer = nullptr;
    static inline std::atomic<bool> s_running{false};
    static inline std::thread s_encodeThread;

    static inline int s_previewWidth = 640;
    static inline int s_previewHeight = 480;
    static inline int s_previewFps = 15;
    static inline int s_jpegQuality = 70;

    static void encodeLoop() {
        const auto interval = std::chrono::microseconds(1'000'000 / s_previewFps);
        auto next = std::chrono::steady_clock::now();

        std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, s_jpegQuality};

        while (s_running) {
            auto maybe = s_frameBuffer->pop(); // blocking
            if (!maybe) continue;

            auto now = std::chrono::steady_clock::now();
            auto wallNow = std::chrono::system_clock::now();

            if (now < next) continue; // simple FPS gate
            next = now + interval;

            cv::Mat resized;
            cv::resize(maybe->frame, resized,
                       cv::Size{s_previewWidth, s_previewHeight},
                       0, 0, cv::INTER_NEAREST);

            std::time_t wallTime = std::chrono::system_clock::to_time_t(wallNow);
            std::tm tm{};
            localtime_r(&wallTime, &tm);

            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

            const double scale = static_cast<double>(resized.cols) / 360.0;

            const double fontScale = std::clamp(scale, 0.5, 4.0);
            const int margin = static_cast<int>(10 * fontScale);
            const int thickness = std::max(1, static_cast<int>(2 * fontScale));

            cv::putText(
                resized,
                oss.str(),
                cv::Point{margin, margin + static_cast<int>(25 * fontScale)},
                cv::FONT_HERSHEY_PLAIN,
                fontScale,
                cv::Scalar{255, 0, 0},
                thickness,
                cv::LINE_8
            );

            std::vector<uint8_t> buf;
            if (!cv::imencode(".jpg", resized, buf, jpegParams)) {
                spdlog::warn("JPEG encode failed");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(s_jpegMutex);
                s_latestJpeg.swap(buf);
                s_jpegSequence.fetch_add(1, std::memory_order_relaxed);
            }
        }
        spdlog::info("MJPEG encode thread exited");
    }

    // ------------------------------------------------------------------
    //  Logging helpers (unchanged)
    // ------------------------------------------------------------------
    static void custom_access_log(const std::string &log_entry) {
        spdlog::debug("http request: {}", log_entry);
    }

    static void custom_error_log(const std::string &log_entry) {
        spdlog::error("http error: {}", log_entry);
    }

    // ------------------------------------------------------------------
    //  JSON helpers (unchanged)
    // ------------------------------------------------------------------
    static json diskUsageJson() {
        struct statvfs stat{};
        if (statvfs(s_dataPath.c_str(), &stat) != 0) {
            return json{{"error", "statvfs failed"}, {"path", s_dataPath}};
        }
        uint64_t totalBytes = stat.f_blocks * stat.f_frsize;
        uint64_t freeBytes = stat.f_bavail * stat.f_frsize;
        uint64_t usedBytes = totalBytes - freeBytes;
        return json{
            {"path", s_dataPath},
            {"total_bytes", totalBytes},
            {"used_bytes", usedBytes},
            {"free_bytes", freeBytes},
            {
                "used_percent",
                totalBytes ? 100.0 * (static_cast<double>(usedBytes) / static_cast<double>(totalBytes)) : 0.0
            }
        };
    }

    static json statusJson() {
        return json{{"recording", VideoRecordThread::isRecording()}};
    }

    static http_response jsonResponse(const json &j, const int code = 200) {
        return http_response::string(j.dump())
                .with_header("Content-Type", "application/json")
                .with_status(code);
    }

    static http_response errorResponse(const std::string &message) {
        return jsonResponse(json{{"error", message}}, 400);
    }

    // ------------------------------------------------------------------
    //  Existing resources (unchanged)
    // ------------------------------------------------------------------
    class StatusResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            return jsonResponse(statusJson());
        }
    };

    class RecordingResource : public http_resource {
    public:
        http_response render_post(const http_request &req) override {
            auto body = std::string(req.get_content());
            json parsed;
            try {
                parsed = json::parse(body);
            } catch (const json::parse_error &e) {
                spdlog::warn("HTTP: malformed /recording body: {}", e.what());
                return errorResponse("invalid JSON body");
            }
            if (!parsed.contains("action") || !parsed["action"].is_string()) {
                return errorResponse("missing or invalid 'action' field");
            }
            const std::string action = parsed["action"].get<std::string>();
            if (action == "start") {
                if (!VideoRecordThread::isRecording()) {
                    VideoRecordThread::setRecording(true);
                    spdlog::info("HTTP: start recording requested");
                }
            } else if (action == "stop") {
                if (VideoRecordThread::isRecording()) {
                    VideoRecordThread::setRecording(false);
                    spdlog::info("HTTP: stop recording requested");
                }
            } else {
                return errorResponse("unknown action: " + action);
            }
            return jsonResponse(statusJson());
        }

        http_response render_get(const http_request &) override {
            return jsonResponse(statusJson());
        }
    };

    class DiskResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            return jsonResponse(diskUsageJson());
        }
    };

    class IndexResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            return http_response::file("index.html")
                    .with_header("Content-Type", "text/html");
        }
    };

    class StreamResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            auto state = std::make_shared<ProducerState>();

            auto producer = [state](std::uint64_t, char *buf, const std::size_t max) -> ssize_t {
                // serve leftover bytes
                if (state->offset < state->part.size()) {
                    const std::size_t avail = state->part.size() - state->offset;
                    const std::size_t n = std::min(avail, max);
                    std::memcpy(buf, state->part.data() + state->offset, n);
                    state->offset += n;
                    return static_cast<ssize_t>(n);
                }

                // need a new multipart part
                if (!s_running.load()) {
                    return -1; // MHD_CONTENT_READER_END_OF_STREAM
                }

                // wait briefly for a newer frame
                uint64_t seq = s_jpegSequence.load(std::memory_order_relaxed);
                for (int i = 0; i < 30 && seq == state->lastSeq; ++i) {
                    if (!s_running.load()) return -1;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    seq = s_jpegSequence.load(std::memory_order_relaxed);
                }
                state->lastSeq = seq;

                std::vector<uint8_t> jpeg;
                {
                    std::lock_guard<std::mutex> lock(s_jpegMutex);
                    jpeg = s_latestJpeg;
                }

                // Build the multipart part into state->part
                state->part.clear();
                if (jpeg.empty()) {
                    state->part =
                            "--frame\r\n"
                            "Content-Type: text/plain\r\n\r\n"
                            "waiting\r\n";
                } else {
                    state->part.reserve(jpeg.size() + 128);
                    state->part += "--frame\r\n";
                    state->part += "Content-Type: image/jpeg\r\n";
                    state->part += "Content-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n";
                    state->part.append(reinterpret_cast<const char *>(jpeg.data()), jpeg.size());
                    state->part += "\r\n";
                }
                state->offset = 0;

                // Now serve the first chunk of the new part
                std::size_t n = std::min(state->part.size(), max);
                std::memcpy(buf, state->part.data(), n);
                state->offset = n;
                return static_cast<ssize_t>(n);
            };

            return http_response::deferred(std::move(producer))
                    .with_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
                    .with_header("Cache-Control", "no-cache, no-store, must-revalidate")
                    .with_header("Pragma", "no-cache")
                    .with_header("Connection", "close");
        }

    private:
        struct ProducerState {
            std::string part;
            std::size_t offset = 0;
            uint64_t lastSeq = 0;
        };
    };

    static inline std::unique_ptr<webserver> s_ws;
    static inline std::string s_dataPath;
};

#endif // TAPS_CAMERANODE_HTTP_SERVER_H
