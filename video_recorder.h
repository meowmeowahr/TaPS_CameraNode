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
                               outputFile, flags.width, flags.height, flags.fps, flags.encoderType, flags.encoderArgs, flags.encoderThreads);
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
    struct Job {
        uint64_t frameIdx{};
        int64_t ptpNs{};
        cv::Mat frame;
    };

    struct Result {
        uint64_t frameIdx{};
        int64_t ptpNs{};
        std::vector<unsigned char> jpegData; // holds either JPEG or raw bytes
    };

    class WorkerInbox {
    public:
        void push(Job &&job) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(job));
            m_cv.notify_one();
        }

        std::optional<Job> pop() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_closed || !m_queue.empty(); });
            if (m_queue.empty()) return std::nullopt;
            Job job = std::move(m_queue.front());
            m_queue.pop_front();
            return job;
        }

        void close() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cv.notify_all();
        }

    private:
        std::deque<Job> m_queue;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_closed = false;
    };

    class ResultBuffer {
    public:
        void push(Result &&r) {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(r));
            m_cv.notify_one();
        }

        std::optional<Result> pop() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [&] { return m_closed || !m_queue.empty(); });
            if (m_queue.empty()) return std::nullopt;
            Result r = std::move(m_queue.front());
            m_queue.pop_front();
            return r;
        }

        void close() {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_closed = true;
            m_cv.notify_all();
        }

    private:
        std::deque<Result> m_queue;
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
            if (const size_t colonPos = pair.find(':'); colonPos != std::string::npos) {
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
                         const std::string& encoderArgsStr,
                         const unsigned char numEncoders) {
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

        // Default values
        int jpegQuality = 85;
        int colorOrder = 0; // 0: RGB, 1: BGR, 2: GRAY, 3: BGR565, 4: BGR555

        // Parse encoder arguments
        for (auto argsMap = parseEncoderArgs(encoderArgsStr); const auto&[arg, val] : argsMap) {
            if (encoderType == EncoderType::JPEG && arg == "quality") {
                try {
                    if (int q = std::stoi(val); q >= 0 && q <= 100) {
                        jpegQuality = q;
                    } else {
                        spdlog::warn("JPEG quality {} out of range [0,100], using default {}", q, jpegQuality);
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Invalid JPEG value '{}': {}", val, e.what());
                }
            } else if (encoderType == EncoderType::RAW && arg == "order") {
                if (val == "rgb") {
                    colorOrder = 0;
                } else if (val == "bgr") {
                    colorOrder = 1;
                } else if (val == "gray") {
                    colorOrder = 2;
                } else if (val == "bgr565") {
                    colorOrder = 3;
                } else if (val == "bgr555") {
                    colorOrder = 4;
                } else {
                    spdlog::warn("Unknown color order '{}', using default RGB", val);
                }
            }
        }

        std::vector<WorkerInbox> inboxes(numEncoders);
        std::vector<ResultBuffer> results(numEncoders);

        std::vector<std::thread> encoders;
        encoders.reserve(numEncoders);

        for (unsigned i = 0; i < numEncoders; ++i) {
            encoders.emplace_back([&, i, encoderType, jpegQuality, colorOrder]() {
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
                while (const auto job = inboxes[i].pop()) {
                    Result r;
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
                            default:
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
                const auto r = results[rr].pop();
                if (!r) break; // that worker is done and drained -> pipeline finished
                auto size = static_cast<uint32_t>(r->jpegData.size());
                output.write(reinterpret_cast<const char *>(&r->frameIdx), sizeof(r->frameIdx));
                output.write(reinterpret_cast<const char *>(&r->ptpNs), sizeof(r->ptpNs));
                output.write(reinterpret_cast<const char *>(&size), sizeof(size));
                output.write(reinterpret_cast<const char *>(r->jpegData.data()), size);
                ++writtenCount;
                rr = (rr + 1) % numEncoders;
            }
        });

        // --- Dispatcher (this function, main loop): round-robin frames to inboxes ---
        uint64_t frameIdx = 0;
        unsigned nextWorker = 0;
        while (auto item = buffer.pop()) {
            if (item->frame.empty()) continue;
            Job job;
            job.frameIdx = frameIdx++;
            job.ptpNs = item->ptpTimestamp.count();
            job.frame = item->frame; // cv::Mat is a shallow/ref-counted handle
            inboxes[nextWorker].push(std::move(job));
            nextWorker = (nextWorker + 1) % numEncoders;
        }

        for (auto &inbox: inboxes) inbox.close();
        for (auto &t: encoders) t.join();
        writerThread.join();

        output.close();

        spdlog::info("Wrote {} frames to {}",
                     writtenCount, outputFile);
    }

    static inline VideoBuffer<TimestampedFrame> *s_buffer = nullptr;
    static inline std::thread s_thread;
};

#endif //TAPS_CAMERANODE_VIDEO_RECORDER_H