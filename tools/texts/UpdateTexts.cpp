// Writes lang/launcher.pot from the texts in src: update-texts <source folder>.
// Built and run by `cmake --build --preset release --target update-texts`.

#include <cstdio>
#include <fstream>

#include "TextScan.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: update-texts <source folder>\n");
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const auto texts = texts::CollectTexts(root);
    const auto pot = texts::MakePot(texts);
    const auto file = root / "lang" / "launcher.pot";
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary);
    stream << pot;
    if (!stream) {
        std::fprintf(stderr, "couldn't write %s\n", file.string().c_str());
        return 1;
    }
    std::printf("%zu texts written to %s\n", texts.size(), file.string().c_str());
    return 0;
}
