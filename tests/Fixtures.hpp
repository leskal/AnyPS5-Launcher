#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <zarchive/zarchivewriter.h>

namespace fixture {

inline std::vector<std::uint8_t> Bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

inline std::vector<std::uint8_t> Elf64() {
    std::vector<std::uint8_t> head(64, 0);
    head[0] = 0x7F;
    head[1] = 'E';
    head[2] = 'L';
    head[3] = 'F';
    head[4] = 2;
    head[5] = 1;
    head[6] = 1;
    head[0x12] = 62;
    return head;
}

inline std::vector<std::uint8_t> Self() {
    std::vector<std::uint8_t> head(64, 0);
    head[0] = 0x4F;
    head[1] = 0x15;
    head[2] = 0x3D;
    head[3] = 0x1D;
    return head;
}

inline std::string ParamJson(const std::string& titleId, const std::string& name, const std::string& version = "01.000.000") {
    return R"({"titleId":")" + titleId + R"(","contentVersion":")" + version +
           R"(","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":")" + name + R"("}}})";
}

using Tree = std::map<std::string, std::vector<std::uint8_t>>;

inline Tree Game(const std::string& titleId, const std::string& name) {
    return {
        {"eboot.bin", Elf64()},
        {"sce_sys/param.json", Bytes(ParamJson(titleId, name))},
        {"sce_sys/icon0.png", Bytes("not really a png")},
        {"Media/data.bin", Bytes("payload")},
    };
}

inline Tree Prefixed(const Tree& tree, const std::string& prefix) {
    Tree result;
    for (const auto& [path, bytes] : tree) result[prefix + path] = bytes;
    return result;
}

inline void WriteFolder(const std::filesystem::path& root, const Tree& tree) {
    for (const auto& [path, bytes] : tree) {
        const auto target = root / path;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream file(target, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

inline void WriteZar(const std::filesystem::path& file, const Tree& tree) {
    struct Context {
        std::filesystem::path path;
        std::ofstream stream;
    } context{file, {}};
    std::filesystem::create_directories(file.parent_path());
    ZArchiveWriter writer(
        [](const int32_t, void* raw) {
            auto* ctx = static_cast<Context*>(raw);
            ctx->stream = std::ofstream(ctx->path, std::ios::binary);
        },
        [](const void* data, size_t length, void* raw) {
            static_cast<Context*>(raw)->stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
        },
        &context);
    for (const auto& [path, bytes] : tree) {
        const auto parent = std::filesystem::path(path).parent_path().generic_string();
        if (!parent.empty()) writer.MakeDir(parent.c_str(), true);
        writer.StartNewFile(path.c_str());
        writer.AppendData(bytes.data(), bytes.size());
    }
    writer.Finalize();
}

class TempDir {
public:
    TempDir() {
        std::random_device device;
        path_ = std::filesystem::temp_directory_path() / ("anyps5-launcher-test-" + std::to_string(device()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

}
