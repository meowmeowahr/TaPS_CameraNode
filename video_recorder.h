//
// Created by kevin on 7/31/26.
//

#ifndef TAPS_CAMERANODE_VIDEO_RECORDER_H
#define TAPS_CAMERANODE_VIDEO_RECORDER_H

#include <chrono>
#include <thread>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <optional>
#include <sstream>
#include <map>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <spdlog/spdlog.h>
#include "runtime_args.h"
#include "video_queue.h"

struct TimestampedFrame {
    cv::Mat frame;
    std::chrono::nanoseconds ptpTimestamp;
};

class VideoRecordThread {
public:
    static void begin(VideoBuffer<TimestampedFrame> *frameBuffer,
                      const std::string &outputFile,
                      const RuntimeArgs &flags) {
        s_buffer = frameBuffer;
        s_thread = std::thread(recorder, std::ref(*frameBuffer),
                               outputFile, flags.width, flags.height, flags.fps, flags.encoderType, flags.encoderArgs);
    }

    static void shutdown() {
        if (s_buffer) {
            s_buffer->shutdown();
        }
        if (s_thread.joinable()) {
            s_thread.join();
        }
    }

private:
    struct RawJob {
        uint64_t frameIdx;
        int64_t ptpNs;
        cv::Mat frame;
    };

    struct EncodedResult {
        uint64_t frameIdx;
        int64_t ptpNs;
        std::vector<unsigned char> jpegData; // holds either JPEG or raw bytes
    };

    class WorkerInbox {
    public:
        void push(RawJob &&job) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(job));
            m_cv.notify_one();
        }

        std::optional<RawJob> pop() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_closed || !m_queue.empty(); });
            if (m_queue.empty()) return std::nullopt;
            RawJob job = std::move(m_queue.front());
            m_queue.pop_front();
            return job;
        }

        void close() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cv.notify_all();
        }

    private:
        std::deque<RawJob> m_queue;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_closed = false;
    };

    // Each encoder writes finished results here in the order it received
    // them; the merge step below round-robins across workers in the same
    // order frames were dispatched, so output stays sequential.
    class ResultBuffer {
    public:
        void push(EncodedResult &&r) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(r));
            m_cv.notify_one();
        }

        std::optional<EncodedResult> pop() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_closed || !m_queue.empty(); });
            if (m_queue.empty()) return std::nullopt;
            EncodedResult r = std::move(m_queue.front());
            m_queue.pop_front();
            return r;
        }

        void close() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cv.notify_all();
        }

    private:
        std::deque<EncodedResult> m_queue;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_closed = false;
    };

    // Helper function to parse encoder args string into a map
    static std::map<std::string, std::string> parseEncoderArgs(const std::string& argsStr) {
        std::map<std::string, std::string> argsMap;
        if (argsStr.empty()) {
            return argsMap;
        }

        std::stringstream ss(argsStr);
        std::string pair;
        while (std::getline(ss, pair, ',')) {
            size_t colonPos = pair.find(':');
            if (colonPos != std::string::npos) {
                std::string key = pair.substr(0, colonPos);
                std::string value = pair.substr(colonPos + 1);
                // Trim whitespace
                size_t start = key.find_first_not_of(" \t");
                size_t end = key.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    key = key.substr(start, end - start + 1);
                } else {
                    key.clear();
                }

                start = value.find_first_not_of(" \t");
                end = value.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    value = value.substr(start, end - start + 1);
                } else {
                    value.clear();
                }

                if (!key.empty()) {
                    argsMap[key] = value;
                }
            }
        }
        return argsMap;
    }

    static void recorder(VideoBuffer<TimestampedFrame> &buffer,
                         const std::string &outputFile,
                         int width, int height, double targetFps,
                         EncoderType encoderType,
                         const std::string& encoderArgsStr) {
        spdlog::info("Start recorder thread targeting {}", outputFile);

        std::ofstream output(outputFile, std::ios::binary);
        if (!output.is_open()) {
            spdlog::error("Failed to open output file: {}", outputFile);
            return;
        }

        // Write header
        std::string header = "TaPS";
        header.push_back(0x01);
        header.push_back(static_cast<char>(encoderType));
        output.write(header.c_str(), static_cast<std::streamsize>(header.length()));
        output.write(reinterpret_cast<const char *>(&width), sizeof(width));
        output.write(reinterpret_cast<const char *>(&height), sizeof(height));
        output.write(reinterpret_cast<const char *>(&targetFps), sizeof(targetFps));
        unsigned int argsLength = encoderArgsStr.length();
        output.write(reinterpret_cast<const char *>(&argsLength), sizeof(unsigned int));
        output.write(encoderArgsStr.c_str(), static_cast<std::streamsize>(encoderArgsStr.length()));

        std::string csvPath = outputFile + ".timing.csv";
        std::ofstream ptpLog(csvPath);
        if (ptpLog.is_open()) {
            ptpLog << "frame_index,ptp_ns\n";
        } else {
            spdlog::warn("Could not open PTP sidecar log: {}", csvPath);
        }

        // Default values
        int jpegQuality = 85;
        int colorOrder = 0; // 0: RGB, 1: BGR, 2: GRAY, 3: BGR565, 4: BGR555

        // Parse encoder arguments
        auto argsMap = parseEncoderArgs(encoderArgsStr);
        for (const auto& arg : argsMap) {
            if (encoderType == EncoderType::JPEG && arg.first == "quality") {
                try {
                    int q = std::stoi(arg.second);
                    if (q >= 0 && q <= 100) {
                        jpegQuality = q;
                    } else {
                        spdlog::warn("JPEG quality {} out of range [0,100], using default {}", q, jpegQuality);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Invalid JPEG value '{}': {}", arg.second, e.what());
                }
            } else if (encoderType == EncoderType::RAW && arg.first == "order") {
                if (arg.second == "rgb") {
                    colorOrder = 0;
                } else if (arg.second == "bgr") {
                    colorOrder = 1;
                } else if (arg.second == "gray") {
                    colorOrder = 2;
                } else if (arg.second == "bgr565") {
                    colorOrder = 3;
                } else if (arg.second == "bgr555") {
                    colorOrder = 4;
                } else {
                    spdlog::warn("Unknown color order '{}', using default RGB", arg.second);
                }
            }
        }

        constexpr unsigned kNumEncoders = 4;

        std::vector<WorkerInbox> inboxes(kNumEncoders);
        std::vector<ResultBuffer> results(kNumEncoders);

        // --- Encoder pool: worker i only ever reads inboxes[i], writes results[i] ---
        std::vector<std::thread> encoders;
        encoders.reserve(kNumEncoders);
        for (unsigned i = 0; i < kNumEncoders; ++i) {
            encoders.emplace_back([&, i, encoderType, jpegQuality, colorOrder]() {
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
                while (auto job = inboxes[i].pop()) {
                    EncodedResult r;
                    r.frameIdx = job->frameIdx;
                    r.ptpNs = job->ptpNs;
                    if (encoderType == EncoderType::JPEG) {
                        cv::imencode(".jpg", job->frame, r.jpegData, params);
                    } else { // RAW
                        // Ensure the matrix is continuous for easy copying
                        cv::Mat img;
                        if (job->frame.isContinuous()) {
                            img = job->frame;
                        } else {
                            img = job->frame.clone();
                        }

                        // Process based on color order
                        switch (colorOrder) {
                            case 0: // RGB
                                if (img.channels() == 3) {
                                    cv::cvtColor(img, img, cv::COLOR_BGR2RGB);
                                }
                                break;
                            case 1: // BGR (default OpenCV)
                                // No conversion needed
                                break;
                            case 2: // GRAY
                                if (img.channels() == 3) {
                                    cv::cvtColor(img, img, cv::COLOR_BGR2GRAY);
                                }
                                break;
                            case 3: // BGR565
                                if (img.channels() == 3) {
                                    cv::cvtColor(img, img, cv::COLOR_BGR2BGR565);
                                }
                                break;
                            case 4: // BGR555
                                if (img.channels() == 3) {
                                    cv::cvtColor(img, img, cv::COLOR_BGR2BGR555);
                                }
                                break;
                        }

                        // Copy the raw data
                        r.jpegData.assign(img.datastart, img.dataend);
                    }
                    results[i].push(std::move(r));
                }
                results[i].close();
            });
        }

        // --- Writer thread: round-robins the same order frames were dispatched in ---
        uint64_t writtenCount = 0;
        std::thread writerThread([&]() {
            unsigned rr = 0;
            while (true) {
                auto r = results[rr].pop();
                if (!r) break; // that worker is done and drained -> pipeline finished
                uint32_t size = static_cast<uint32_t>(r->jpegData.size());
                output.write(reinterpret_cast<const char *>(&r->frameIdx), sizeof(r->frameIdx));
                output.write(reinterpret_cast<const char *>(&r->ptpNs), sizeof(r->ptpNs));
                output.write(reinterpret_cast<const char *>(&size), sizeof(size));
                output.write(reinterpret_cast<const char *>(r->jpegData.data()), size);
                if (ptpLog.is_open()) {
                    ptpLog << r->frameIdx << "," << r->ptpNs << "\n";
                }
                ++writtenCount;
                rr = (rr + 1) % kNumEncoders;
            }
        });

        // --- Dispatcher (this function, main loop): round-robin frames to inboxes ---
        uint64_t frameIdx = 0;
        unsigned nextWorker = 0;
        while (auto item = buffer.pop()) {
            if (item->frame.empty()) continue;
            RawJob job;
            job.frameIdx = frameIdx++;
            job.ptpNs = item->ptpTimestamp.count();
            job.frame = item->frame; // cv::Mat is a shallow/ref-counted handle
            inboxes[nextWorker].push(std::move(job));
            nextWorker = (nextWorker + 1) % kNumEncoders;
        }

        for (auto &inbox: inboxes) inbox.close();
        for (auto &t: encoders) t.join();
        writerThread.join();

        output.close();
        if (ptpLog.is_open()) ptpLog.close();

        spdlog::info("Recorder finished. Wrote {} frames to {} (and {})",
                     writtenCount, outputFile, csvPath);
    }

    static inline VideoBuffer<TimestampedFrame> *s_buffer = nullptr;
    static inline std::thread s_thread;
};

#endif //TAPS_CAMERANODE_VIDEO_RECORDER_H