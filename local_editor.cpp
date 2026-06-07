#include "args.hpp"
#include "editor_app.hpp"

#include <exception>
#include <iostream>
#include <utility>

#ifdef _WIN32
#include <windows.h>
// Optimus/Enduro laptops can create the on-screen GL context on the integrated
// GPU, which renders the VTK window black even though off-screen FBO rendering
// works. Exporting these symbols forces the high-performance discrete GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char** argv) {
  try {
    tracto::Args args = tracto::ParseArgs(argc, argv);
    tracto::EditorApp app(std::move(args));
    app.Load();
    app.Run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
