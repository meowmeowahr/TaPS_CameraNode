//
// Created by kevin on 8/5/26.
//

#ifndef TAPS_CAMERANODE_TAPS_READER_H
#define TAPS_CAMERANODE_TAPS_READER_H

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

constexpr char file_magic[5] = {'T', 'a', 'P', 'S', 0x02};

class TaPS_Reader {
public:
    enum class EncoderType : uint8_t {
        JPEG = 0x00,
        RAW = 0x01
    };

#pragma pack(push, 1)
    struct RawHeader {
        char signature[5];
        EncoderType encoding;
        uint64_t width;
        uint64_t height;
        double target_fps;
        uint32_t encoder_args_length;
    };

    struct FrameHeader {
        uint64_t frame_idx;
        int64_t nanoseconds;
        uint32_t frame_size;
    };
#pragma pack(pop)

    struct Header {
        EncoderType encoding;
        uint64_t width;
        uint64_t height;
        double target_fps;
        std::string encoder_args;
        uint64_t frame_count;
    };

    struct Frame {
        FrameHeader header;
        std::vector<uint8_t> data;
    };

    explicit TaPS_Reader(const fs::path &path) {
        file_stream.open(path, std::ios::binary);
        if (!file_stream.is_open()) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        header = read_header();
        first_frame_offset = file_stream.tellg();
    }

    Header read_header() {
        if (!file_stream.is_open()) {
            throw std::runtime_error("File is closed");
        }

        file_stream.seekg(0);

        RawHeader raw{};
        file_stream.read(reinterpret_cast<char *>(&raw), sizeof(RawHeader));
        if (!file_stream) {
            throw std::runtime_error("Failed to read raw header fields");
        }

        if (std::memcmp(raw.signature, file_magic, 5) != 0) {
            throw std::runtime_error("File does not contain the TaPS v2 header sequence");
        }

        Header parsed{};
        parsed.encoding = raw.encoding;
        parsed.width = raw.width;
        parsed.height = raw.height;
        parsed.target_fps = raw.target_fps;

        parsed.encoder_args.resize(raw.encoder_args_length);
        if (raw.encoder_args_length > 0) {
            file_stream.read(&parsed.encoder_args[0], raw.encoder_args_length);
            if (!file_stream) {
                throw std::runtime_error("Failed to read encoder args string");
            }
        }

        file_stream.read(reinterpret_cast<char *>(&parsed.frame_count), sizeof(parsed.frame_count));
        if (!file_stream) {
            throw std::runtime_error("Failed to read frame count");
        }

        return parsed;
    }

    bool read_next_frame(Frame &frame) {
        if (!file_stream.is_open()) {
            throw std::runtime_error("File is closed");
        }

        file_stream.read(reinterpret_cast<char *>(&frame.header), sizeof(FrameHeader));
        if (file_stream.gcount() == 0) {
            return false; // eof
        }
        if (!file_stream) {
            throw std::runtime_error("Incomplete frame header read");
        }

        frame.data.resize(frame.header.frame_size);
        if (frame.header.frame_size > 0) {
            file_stream.read(reinterpret_cast<char *>(frame.data.data()), frame.header.frame_size);
            if (!file_stream) {
                throw std::runtime_error("Incomplete frame payload read");
            }
        }

        return true;
    }

    void seek_to_first_frame() {
        if (!file_stream.is_open()) {
            throw std::runtime_error("File is closed");
        }
        file_stream.clear();
        file_stream.seekg(first_frame_offset, std::ios::beg);
    }

    void seek(const uint64_t target_frame_idx) {
        if (!file_stream.is_open()) {
            throw std::runtime_error("File is closed");
        }

        seek_to_first_frame();

        FrameHeader frame_hdr{};
        while (file_stream) {
            const std::streampos frame_start = file_stream.tellg();

            file_stream.read(reinterpret_cast<char *>(&frame_hdr), sizeof(FrameHeader));
            if (!file_stream) {
                throw std::runtime_error("Target frame index out of range or file corrupted");
            }

            if (frame_hdr.frame_idx == target_frame_idx) {
                file_stream.seekg(frame_start, std::ios::beg);
                return;
            }

            file_stream.seekg(frame_hdr.frame_size, std::ios::cur);
        }

        throw std::runtime_error("Frame index not found");
    }

    const Header &get_header() const { return header; }

    void close() {
        file_stream.close();
    }

private:
    std::ifstream file_stream;
    Header header{};
    std::streampos first_frame_offset{0};
};


#endif //TAPS_CAMERANODE_TAPS_READER_H
