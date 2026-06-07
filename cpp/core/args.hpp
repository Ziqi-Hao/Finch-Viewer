#pragma once

#include <cstdint>
#include <string>

namespace tracto {

struct Args {
  std::string volumePath;
  std::string trkPath;
  std::string outPath;
  int displayN = 12000;
  int dispStep = 2;
  uint64_t seed = 0;
  bool noVolume = false;
  bool frontBuffer = false;
  bool gdiBlit = false;
  std::string screenshotPath;
};

void Usage(const char* exe);
Args ParseArgs(int argc, char** argv);

}  // namespace tracto
