#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "core/SelfUnwrap.hpp"

namespace {

int Unwrap(const std::string& input, const std::string& output, std::ostream& report) {
    std::vector<std::uint8_t> data;
    {
        std::ifstream file(input, std::ios::binary);
        if (!file) {
            report << "ERROR: cannot open input\n";
            return 3;
        }
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    try {
        const auto image = launcher::UnwrapSelfImage(data);
        std::ofstream(output, std::ios::binary).write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        report << "OK " << image.size() << "\n";
        return 0;
    } catch (const launcher::UnwrapRefused& refused) {
        report << "REFUSED: " << refused.what() << "\n";
        return 2;
    } catch (const std::exception& failure) {
        report << "ERROR: " << failure.what() << "\n";
        return 3;
    }
}

}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--batch") {
        std::ifstream list(argv[2]);
        std::string input;
        std::size_t index = 0;
        while (std::getline(list, input)) {
            Unwrap(input, std::string(argv[3]) + "/" + std::to_string(index++) + ".elf", std::cout);
        }
        return 0;
    }
    if (argc != 3) {
        std::cerr << "usage: unwrap_self_cli <input.self> <output.elf>\n       unwrap_self_cli --batch <list> <output-dir>\n";
        return 1;
    }
    return Unwrap(argv[1], argv[2], std::cout);
}
