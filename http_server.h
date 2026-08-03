//
// Created by kevin on 7/31/26.
//
#ifndef TAPS_CAMERANODE_HTTP_SERVER_H
#define TAPS_CAMERANODE_HTTP_SERVER_H

#include <string>
#include <sys/statvfs.h>
#include <httpserver.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "runtime_args.h"
#include "video_recorder.h"

using namespace httpserver;
using json = nlohmann::json;

class HttpServer {
public:
    static void begin(const RuntimeArgs &flags) {
        s_dataPath = flags.outputDir.empty() ? "/" : flags.outputDir;

        s_ws = std::make_unique<webserver>(
            create_webserver(flags.httpPort).log_access(custom_access_log).log_error(custom_error_log));

        s_ws->register_path("/status", std::make_unique<StatusResource>());
        s_ws->register_path("/recording", std::make_unique<RecordingResource>());
        s_ws->register_path("/disk", std::make_unique<DiskResource>());
        s_ws->register_path("/", std::make_unique<IndexResource>());

        s_ws->start(false);
        spdlog::info("HTTP server started on port {}", flags.httpPort);
    }

    static void stop() {
        if (s_ws) {
            s_ws->stop();
            s_ws.reset();
        }
    }

private:
    static void custom_access_log(const std::string &log_entry) {
        spdlog::debug("http request: {}", log_entry);
    }

    static void custom_error_log(const std::string &log_entry) {
        spdlog::error("http error: {}", log_entry);
    }

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
            {"used_percent", totalBytes ? 100.0 * (static_cast<double>(usedBytes) / totalBytes) : 0.0}
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

    static http_response errorResponse(const std::string &message, int code = 400) {
        return jsonResponse(json{{"error", message}}, code);
    }

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
                return errorResponse("invalid JSON body", 400);
            }

            if (!parsed.contains("action") || !parsed["action"].is_string()) {
                return errorResponse("missing or invalid 'action' field", 400);
            }

            if (const std::string action = parsed["action"].get<std::string>(); action == "start") {
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
                return errorResponse("unknown action: " + action, 400);
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

    static inline std::unique_ptr<webserver> s_ws;
    static inline std::string s_dataPath;
};

#endif //TAPS_CAMERANODE_HTTP_SERVER_H
