#pragma once

#include <cstdint>
#include <string>

namespace tracto {

struct Args {
  std::string volumePath;
  std::string labelPath;   // integer-label / segmentation NIfTI (label-coloured)
  std::string odfPath;     // 4-D SH-coefficient ODF NIfTI (rendered as glyphs)
  std::string discreteOdfPath;  // 4-D sphere-sampled ODF, glyphs via embedded sphere (opt-in/unverified)
  std::string peaksPath;   // 4-D peaks NIfTI (rendered as DEC line segments)
  std::string openPath;    // any file, routed through the unified auto-detect loader
  std::string trkPath;
  std::string outPath;
  int displayN = 12000;
  int dispStep = 2;
  uint64_t seed = 0;
  bool noVolume = false;
  bool frontBuffer = false;
  bool gdiBlit = false;
  std::string screenshotPath;
  std::string windowShotPath;  // full-window grab (toolbar + docks), for UI verification
  bool hasSliceZ = false;      // --slice-z: jump the slice focus before the screenshot
  float sliceZ = 0.0f;         // world Z to jump to (verifies slice-following)
};

void Usage(const char* exe);
Args ParseArgs(int argc, char** argv);

}  // namespace tracto
