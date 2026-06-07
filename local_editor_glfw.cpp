#include "args.hpp"
#include "glfw_tract_viewer.hpp"

#include <exception>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
  try {
    tracto::Args args = tracto::ParseArgs(argc, argv);
    tracto::GlfwTractViewer viewer(std::move(args));
    viewer.Load();
    viewer.Run();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}
