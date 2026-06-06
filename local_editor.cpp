#include "args.hpp"
#include "editor_app.hpp"

#include <exception>
#include <iostream>
#include <utility>

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
