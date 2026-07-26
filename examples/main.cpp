#include "VulkanViewer.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* executable)
{
    std::cout
        << "Usage: " << executable << " <model-path> [--frames N]\n"
        << "\n"
        << "The camera automatically orbits the loaded model.\n"
        << "Use the mouse wheel to move the camera closer or farther away.\n"
        << "Press E and enter a path in this console to export the current "
           "model.\n"
        << "--frames is optional and is useful for automated smoke tests.\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "--help" ||
        std::string(argv[1]) == "-h") {
        PrintUsage(argv[0]);
        return argc < 2 ? 1 : 0;
    }

    std::uint64_t frameLimit = 0;
    if (argc == 4 && std::string(argv[2]) == "--frames") {
        try {
            frameLimit = std::stoull(argv[3]);
        } catch (const std::exception&) {
            std::cerr << "Invalid frame count: " << argv[3] << '\n';
            return 1;
        }
    } else if (argc != 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    try {
        VulkanViewer viewer(std::filesystem::path(argv[1]), frameLimit);
        return viewer.Run();
    } catch (const std::exception& error) {
        std::cerr << "Viewer error: " << error.what() << '\n';
        return 1;
    }
}
