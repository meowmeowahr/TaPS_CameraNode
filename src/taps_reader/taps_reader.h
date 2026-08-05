//
// Created by kevin on 8/5/26.
//

#ifndef TAPS_CAMERANODE_TAPS_READER_H
#define TAPS_CAMERANODE_TAPS_READER_H
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

constexpr char file_magic[5] = {'T', 'a', 'P', 'S', 0x02};


class TaPS_Reader {
public:
    enum EncoderType: unsigned char {
        JPEG = 0x00,
        RAW = 0x01
    };

    explicit TaPS_Reader(const fs::path &path) {
        file_stream = std::ifstream(path, std::ios::binary);
        char signature[5];
        file_stream.read(signature, 5);
        if (memcmp(signature, file_magic, 5) != 0) {
            throw std::runtime_error("file does not contain the TaPS v2 header sequence");
        }
    }

    EncoderType get_encoding() {
        if (file_stream.is_open()) {
            char type[1];
            file_stream.seekg(5); // 6th byte
            file_stream.read(type, 1);
            return static_cast<EncoderType>(type[0]);
        }
        throw std::runtime_error("file is closed");;
    }

    unsigned long get_frame_width() {
        if (file_stream.is_open()) {
            unsigned long number;
            file_stream.seekg(6); // 7th byte
            file_stream.read(reinterpret_cast<char*>(&number), sizeof(number));
            return number;
        }
        throw std::runtime_error("file is closed");
    }

    unsigned long get_frame_height() {
        if (file_stream.is_open()) {
            unsigned long number;
            file_stream.seekg(14); // 15th byte
            file_stream.read(reinterpret_cast<char*>(&number), sizeof(number));
            return number;
        }
        throw std::runtime_error("file is closed");
    }

    double get_target_fps() {
        if (file_stream.is_open()) {
            double number;
            file_stream.seekg(0x16);
            file_stream.read(reinterpret_cast<char*>(&number), sizeof(number));
            return number;
        }
        throw std::runtime_error("file is closed");
    }

    void close() {
        file_stream.close();
    }
private:
    std::ifstream file_stream;
};


#endif //TAPS_CAMERANODE_TAPS_READER_H
