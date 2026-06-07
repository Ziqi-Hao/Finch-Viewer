#include "args.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace tracto {

void Usage(const char* exe) {
  std::cout
      << "Usage:\n"
      << "  " << exe << " --fa FA.nii.gz --trk input.trk --out edited.trk\n\n"
      << "Options:\n"
      << "  --display-n N   streamlines drawn interactively, default 12000\n"
      << "  --disp-step N   draw every N-th point per streamline, default 2\n"
      << "  --seed N        deterministic display subsampling seed, default 0\n"
      << "  --no-fa         start without FA slices, useful for display debugging\n"
      << "  --front-buffer  draw directly to the front buffer for display debugging\n"
      << "  --screenshot P  render one frame to PNG and exit\n"
      << "  --help          show this help\n";
}

Args ParseArgs(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    auto requireValue = [&](const std::string& opt) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + opt);
      }
      return argv[++i];
    };

    if (key == "--fa") {
      args.faPath = requireValue(key);
    } else if (key == "--trk") {
      args.trkPath = requireValue(key);
    } else if (key == "--out") {
      args.outPath = requireValue(key);
    } else if (key == "--display-n") {
      args.displayN = std::stoi(requireValue(key));
    } else if (key == "--disp-step") {
      args.dispStep = std::stoi(requireValue(key));
    } else if (key == "--seed") {
      args.seed = static_cast<uint64_t>(std::stoull(requireValue(key)));
    } else if (key == "--no-fa") {
      args.noFa = true;
    } else if (key == "--front-buffer") {
      args.frontBuffer = true;
    } else if (key == "--screenshot") {
      args.screenshotPath = requireValue(key);
    } else if (key == "--help" || key == "-h") {
      Usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + key);
    }
  }

  if ((!args.noFa && args.faPath.empty()) || args.trkPath.empty() || args.outPath.empty()) {
    Usage(argv[0]);
    throw std::runtime_error("missing required --fa, --trk, or --out");
  }
  if (args.displayN <= 0) {
    throw std::runtime_error("--display-n must be positive");
  }
  if (args.dispStep <= 0) {
    throw std::runtime_error("--disp-step must be positive");
  }

  return args;
}

}  // namespace tracto
