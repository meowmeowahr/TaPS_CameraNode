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
#include <opencv2/core/mat.hpp>
#include <opencv2/imgcodecs.hpp>
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
                               outputFile, flags.width, flags.height, flags.fps);
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
        std::vector<uchar> jpegData;
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

    static void recorder(VideoBuffer<TimestampedFrame> &buffer,
                         const std::string &outputFile,
                         int width, int height, double targetFps) {
        spdlog::info("Start recorder thread targeting {}", outputFile);

        std::ofstream output(outputFile, std::ios::binary);
        if (!output.is_open()) {
            spdlog::error("Failed to open output file: {}", outputFile);
            return;
        }

        std::string csvPath = outputFile + ".timing.csv";
        std::ofstream ptpLog(csvPath);
        if (ptpLog.is_open()) {
            ptpLog << "frame_index,ptp_ns\n";
        } else {
            spdlog::warn("Could not open PTP sidecar log: {}", csvPath);
        }

        constexpr unsigned kNumEncoders = 4;
        constexpr int jpegQuality = 85;

        std::vector<WorkerInbox> inboxes(kNumEncoders);
        std::vector<ResultBuffer> results(kNumEncoders);

        // --- Encoder pool: worker i only ever reads inboxes[i], writes results[i] ---
        std::vector<std::thread> encoders;
        encoders.reserve(kNumEncoders);
        for (unsigned i = 0; i < kNumEncoders; ++i) {
            encoders.emplace_back([&, i]() {
                std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, jpegQuality};
                while (auto job = inboxes[i].pop()) {
                    EncodedResult r;
                    r.frameIdx = job->frameIdx;
                    r.ptpNs = job->ptpNs;
                    cv::imencode(".jpg", job->frame, r.jpegData, params);
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
