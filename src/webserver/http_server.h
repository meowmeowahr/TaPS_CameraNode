//
// Created by kevin on 7/31/26.
//
#ifndef TAPS_CAMERANODE_HTTP_SERVER_H
#define TAPS_CAMERANODE_HTTP_SERVER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <sys/statvfs.h>

#include <httpserver.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "../runtime_args.h"
#include "../video_recorder.h"
#include "../video_queue.h"
#include "../taps_reader/taps_reader.h"

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
        s_ws->register_path("/events", std::make_unique<SseResource>());
        s_ws->register_prefix("/files", std::make_unique<FileBrowseResource>(flags.outputDir, "/files"));
        s_ws->register_path("/", std::make_unique<IndexResource>());

        // Start the frame → JPEG pump
        s_running = true;
        s_encodeThread = std::thread(encodeLoop);

        // first cpu sample
        readCpuTicks(s_prevIdle, s_prevTotal);

        s_heartbeatThread = std::thread(heartbeatLoop);

        // Wire recorder state-change callback so we push immediately on transition
        VideoRecordThread::setStateCallback([](VideoRecordThread::RecorderState) {
            broadcastEvent("status", buildStatusPayload());
        });

        s_ws->start(false); // non-blocking
        spdlog::info("HTTP server started on port {} (MJPEG at /stream)", flags.httpPort);
    }

    static void stop() {
        VideoRecordThread::setStateCallback(nullptr);

        s_running = false;
        s_heartbeatCv.notify_all();

        if (s_encodeThread.joinable())
            s_encodeThread.join();
        if (s_heartbeatThread.joinable())
            s_heartbeatThread.join();

        {
            std::lock_guard lock(s_sseMutex);
            for (const auto &client: s_sseClients) {
                std::lock_guard cLock(client->mutex);
                client->closed = true;
                client->cv.notify_all();
            }
            s_sseClients.clear();
        }

        if (s_ws) {
            s_ws->stop();
            s_ws.reset();
        }
    }

private:
    struct SseClient {
        std::mutex mutex;
        std::condition_variable cv;
        std::queue<std::string> messages;
        bool closed = false;
    };

    static inline std::mutex s_sseMutex;
    static inline std::vector<std::shared_ptr<SseClient> > s_sseClients;

    static void readCpuTicks(uint64_t &idle, uint64_t &total) {
        idle = total = 0;
        std::ifstream f("/proc/stat");
        std::string label;
        f >> label; // "cpu"
        uint64_t user, nice, system, idle_v, iowait, irq, softirq, steal;
        f >> user >> nice >> system >> idle_v >> iowait >> irq >> softirq >> steal;
        idle = idle_v + iowait;
        total = user + nice + system + idle_v + iowait + irq + softirq + steal;
    }

    static double cpuUsagePct() {
        uint64_t idle, total;
        readCpuTicks(idle, total);
        const uint64_t dTotal = total - s_prevTotal;
        const uint64_t dIdle = idle - s_prevIdle;
        s_prevTotal = total;
        s_prevIdle = idle;
        if (dTotal == 0) return 0.0;
        return 100.0 * (1.0 - static_cast<double>(dIdle) / static_cast<double>(dTotal));
    }

    static void readMemInfo(uint64_t &totalKb, uint64_t &availKb) {
        totalKb = availKb = 0;
        std::ifstream f("/proc/meminfo");
        std::string key;
        uint64_t val;
        std::string unit;
        int found = 0;
        while (found < 2 && f >> key >> val >> unit) {
            if (key == "MemTotal:") {
                totalKb = val;
                ++found;
            } else if (key == "MemAvailable:") {
                availKb = val;
                ++found;
            }
        }
    }

    static inline uint64_t s_prevIdle = 0;
    static inline uint64_t s_prevTotal = 0;

    static std::string stateString(const VideoRecordThread::RecorderState s) {
        using S = VideoRecordThread::RecorderState;
        switch (s) {
            case S::Idle: return "I";
            case S::Recording: return "R";
            case S::Saving: return "S";
        }
        return "I";
    }

    static json buildStatusPayload() {
        return json{{"s", stateString(VideoRecordThread::getState())}};
    }

    static json buildDiskPayload() {
        struct statvfs st{};
        if (statvfs(s_dataPath.c_str(), &st) != 0)
            return json{{"err", 1}};
        const uint64_t t = st.f_blocks * st.f_frsize;
        const uint64_t f = st.f_bavail * st.f_frsize;
        const uint64_t u = t - f;
        return json{
            {"t", t}, {"u", u}, {"f", f},
            {"p", t ? std::round(1000.0 * u / t) / 10.0 : 0.0},
            {"path", s_dataPath}
        };
    }

    static json buildCpuPayload() {
        return json{{"p", std::round(cpuUsagePct() * 10.0) / 10.0}};
    }

    static json buildMemPayload() {
        uint64_t totalKb, availKb;
        readMemInfo(totalKb, availKb);
        const uint64_t usedKb = totalKb - availKb;
        const double pct = totalKb ? std::round(1000.0 * usedKb / totalKb) / 10.0 : 0.0;
        return json{
            {"t", totalKb * 1024}, {"u", usedKb * 1024},
            {"f", availKb * 1024}, {"p", pct}
        };
    }


    static std::string makeSseFrame(const std::string &eventName, const json &payload) {
        return "event: " + eventName + "\ndata: " + payload.dump() + "\n\n";
    }

    static void broadcastEvent(const std::string &eventName, const json &payload) {
        const std::string frame = makeSseFrame(eventName, payload);
        std::lock_guard lock(s_sseMutex);
        for (auto it = s_sseClients.begin(); it != s_sseClients.end();) {
            const auto &client = *it;
            std::lock_guard cLock(client->mutex);
            if (client->closed) {
                it = s_sseClients.erase(it);
            } else {
                client->messages.push(frame);
                client->cv.notify_all();
                ++it;
            }
        }
    }

    static inline std::mutex s_heartbeatMutex;
    static inline std::condition_variable s_heartbeatCv;
    static inline std::thread s_heartbeatThread;

    static void heartbeatLoop() {
        pthread_setname_np(pthread_self(), "sse_heartbeat");
        while (s_running) {
            {
                std::unique_lock lock(s_heartbeatMutex);
                s_heartbeatCv.wait_for(lock, std::chrono::seconds(1),
                                       [] { return !s_running.load(); });
            }
            if (!s_running) break;

            broadcastEvent("cpu", buildCpuPayload());
            broadcastEvent("mem", buildMemPayload());
            broadcastEvent("disk", buildDiskPayload());
        }
        spdlog::info("SSE heartbeat thread exited");
    }

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
        pthread_setname_np(pthread_self(), "str_encode");
        const auto interval = std::chrono::microseconds(1'000'000 / s_previewFps);
        auto next = std::chrono::steady_clock::now();

        const std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, s_jpegQuality};

        const double scale = static_cast<double>(s_previewWidth) / 360.0;
        const double fontScale = std::clamp(scale, 0.5, 4.0);
        const int thickness = std::max(1, static_cast<int>(2 * fontScale));

        cv::Mat resized;
        while (s_running) {
            const auto maybe = s_frameBuffer->pop(); // blocking
            if (!maybe) continue;

            auto now = std::chrono::steady_clock::now();
            auto wallNow = std::chrono::system_clock::now();

            if (now < next) continue; // simple FPS gate
            next = now + interval;

            cv::resize(maybe->frame, resized,
                       cv::Size{s_previewWidth, s_previewHeight},
                       0, 0, cv::INTER_NEAREST);

            std::string timeStr = std::format("{:%Y-%m-%d %H:%M:%S}", wallNow);

            cv::putText(
                resized,
                timeStr,
                cv::Point{20, resized.rows - 20},
                cv::FONT_HERSHEY_PLAIN,
                fontScale,
                cv::Scalar{255, 0, 0},
                thickness,
                cv::LINE_4
            );

            std::vector<uint8_t> buf;
            if (!cv::imencode(".jpg", resized, buf, jpegParams)) {
                spdlog::warn("JPEG encode failed");
                continue;
            }

            {
                std::lock_guard lock(s_jpegMutex);
                s_latestJpeg.swap(buf);
                s_jpegSequence.fetch_add(1, std::memory_order_relaxed);
            }
        }
        spdlog::info("MJPEG encode thread exited");
    }

    // ------------------------------------------------------------------
    //  Logging helpers
    // ------------------------------------------------------------------
    static void custom_access_log(const std::string &log_entry) {
        spdlog::debug("http request: {}", log_entry);
    }

    static void custom_error_log(const std::string &log_entry) {
        spdlog::error("http error: {}", log_entry);
    }

    // ------------------------------------------------------------------
    //  Legacy JSON helpers (for REST endpoints)
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
                "used_percent", totalBytes
                                    ? 100.0 * (static_cast<double>(usedBytes) / static_cast<double>(totalBytes))
                                    : 0.0
            }
        };
    }

    static json statusJson() {
        return json{{"recording", VideoRecordThread::getState()}};
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
    //  Resources
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
            if (const std::string action = parsed["action"].get<std::string>(); action == "start") {
                if (VideoRecordThread::getState() == VideoRecordThread::RecorderState::Idle) {
                    VideoRecordThread::setRecording(true);
                    spdlog::info("HTTP: start recording requested");
                }
            } else if (action == "stop") {
                if (VideoRecordThread::getState() == VideoRecordThread::RecorderState::Recording) {
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
            return http_response::file("templates/index.html")
                    .with_header("Content-Type", "text/html");
        }
    };

    class FileBrowseResource : public http_resource {
    public:
        FileBrowseResource(fs::path root, std::string mountPrefix)
            : m_root(std::move(root)), m_mountPrefix(std::move(mountPrefix)) {
        }

        http_response render_get(const http_request &req) override {
            fs::path target;
            if (!resolve(req, target)) return http_response::string("Forbidden").with_status(403);

            std::error_code ec;
            if (!fs::exists(target, ec)) {
                return http_response::string("Not found").with_status(404);
            }

            if (fs::is_directory(target, ec)) {
                std::ostringstream out;
                out << "[";
                bool first = true;
                for (const auto &entry: fs::directory_iterator(target, ec)) {
                    if (!first) out << ",";
                    first = false;
                    out << "{\"name\":\"" << entry.path().filename().string() << "\","
                            << "\"is_dir\":" << (entry.is_directory() ? "true" : "false") << "}";
                }
                out << "]";
                return http_response::string(out.str(), "application/json");
            }

            if (req.get_args().contains(std::string_view("thumb"))) {
                auto reader = TaPS_Reader(target);
                auto [encoding, width, height, target_fps, encoder_args, frame_count] = reader.get_header();
                spdlog::debug(std::format(
                    "request for thumbnail {}, encoding {}, width {}, height {}, target fps {}, encoder args {}, frames {}",
                    target.c_str(),
                    static_cast<int>(encoding), width, height, target_fps, encoder_args, frame_count));
                auto frame = TaPS_Reader::Frame{};
                reader.read_next_frame(frame);
                reader.close();
                return http_response::string(std::string(frame.data.begin(), frame.data.end()));
            }

            return http_response::string(target.string(), "application/octet-stream");
        }

        http_response render_delete(const http_request &req) override {
            fs::path target;
            if (!resolve(req, target)) return http_response::string("Forbidden").with_status(403);

            std::error_code ec;
            if (!fs::exists(target, ec)) {
                return http_response::string("Not found").with_status(404);
            }
            if (fs::equivalent(target, m_root, ec)) return http_response::string("Forbidden").with_status(403);
            // don't nuke the root

            if (const bool removed = fs::remove_all(target, ec) > 0 && !ec; !removed) {
                return http_response::string("Delete failed").with_status(500);
            }
            return http_response::empty();
        }

    private:
        fs::path m_root;
        std::string m_mountPrefix; // e.g. "/files"

        // Extracts everything after the mount prefix from req.get_path(), then
        // resolves it against m_root, rejecting any attempt to escape it.
        bool resolve(const http_request &req, fs::path &out) const {
            const auto fullPath = req.get_path(); // e.g. "/files/sub/dir/file.txt"

            std::string rel;
            if (fullPath.size() > m_mountPrefix.size() &&
                fullPath.compare(0, m_mountPrefix.size(), m_mountPrefix) == 0) {
                rel = fullPath.substr(m_mountPrefix.size());
            }
            if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);

            fs::path candidate = m_root / rel;

            std::error_code ec;
            fs::path canonicalRoot = fs::weakly_canonical(m_root, ec);
            fs::path canonicalTarget = fs::weakly_canonical(candidate, ec);

            auto [rootEnd, targetIt] = std::mismatch(
                canonicalRoot.begin(), canonicalRoot.end(), canonicalTarget.begin(), canonicalTarget.end());
            if (rootEnd != canonicalRoot.end()) return false; // escaped the root

            out = canonicalTarget;
            return true;
        }

        bool isWithinRoot(const fs::path &p) const {
            std::error_code ec;
            fs::path canonicalRoot = fs::weakly_canonical(m_root, ec);
            fs::path canonicalTarget = fs::weakly_canonical(p, ec);
            auto [rootEnd, targetIt] = std::mismatch(
                canonicalRoot.begin(), canonicalRoot.end(), canonicalTarget.begin(), canonicalTarget.end());
            return rootEnd == canonicalRoot.end();
        }
    };

    // ------------------------------------------------------------------
    //  SSE Resource -- one long-lived streaming connection per browser tab.
    //  On connect: immediately seeds the client with current state + all
    //  three telemetry readings so the UI is populated before the first
    //  heartbeat fires.
    // ------------------------------------------------------------------
    class SseResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            auto client = std::make_shared<SseClient>();
            {
                std::lock_guard lock(s_sseMutex);
                s_sseClients.push_back(client);
            }

            // Seed initial burst: status + all three telemetry events.
            // CPU is sampled twice quickly so the delta is near-zero on
            // first connect rather than misleadingly high.
            {
                std::lock_guard cLock(client->mutex);
                client->messages.push(makeSseFrame("status", buildStatusPayload()));
                client->messages.push(makeSseFrame("disk", buildDiskPayload()));
                client->messages.push(makeSseFrame("mem", buildMemPayload()));
                // Fire a dummy cpu read to initialise the delta; result discarded.
                uint64_t idle2, total2;
                readCpuTicks(idle2, total2);
                client->messages.push(makeSseFrame("cpu", json{{"p", 0.0}}));
            }

            auto producer = [client](std::uint64_t /*seq*/, char *buf, const std::size_t max) -> ssize_t {
                std::unique_lock lock(client->mutex);
                client->cv.wait(lock, [&] {
                    return client->closed || !client->messages.empty();
                });

                if (client->closed && client->messages.empty())
                    return -1; // EOF -- EventSource will auto-reconnect

                std::string &msg = client->messages.front();
                const std::size_t n = std::min(msg.size(), max);
                std::memcpy(buf, msg.data(), n);
                if (n == msg.size()) {
                    client->messages.pop();
                } else {
                    msg.erase(0, n);
                }
                return static_cast<ssize_t>(n);
            };

            return http_response::deferred(std::move(producer))
                    .with_header("Content-Type", "text/event-stream")
                    .with_header("Cache-Control", "no-cache, no-store, must-revalidate")
                    .with_header("Connection", "keep-alive")
                    .with_header("X-Accel-Buffering", "no"); // disable nginx proxy buffering
        }
    };

    // ------------------------------------------------------------------
    //  MJPEG stream resource (unchanged)
    // ------------------------------------------------------------------
    class StreamResource : public http_resource {
    public:
        http_response render_get(const http_request &) override {
            auto state = std::make_shared<ProducerState>();

            auto producer = [state](std::uint64_t, char *buf, const std::size_t max) -> ssize_t {
                if (state->offset < state->part.size()) {
                    const std::size_t avail = state->part.size() - state->offset;
                    const std::size_t n = std::min(avail, max);
                    std::memcpy(buf, state->part.data() + state->offset, n);
                    state->offset += n;
                    return static_cast<ssize_t>(n);
                }

                if (!s_running.load())
                    return -1;

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

                const std::size_t n = std::min(state->part.size(), max);
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
