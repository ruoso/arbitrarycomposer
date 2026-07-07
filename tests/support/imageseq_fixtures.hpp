#pragma once

// Shared helpers for the org.arbc.imageseq test binaries. The enforcing tests
// live under tests/ (not plugins/) because that is what check_claims.py /
// check_levels.py scan; they link `arbc-plugin-imageseq-impl` to construct the
// content directly, and receive the checked-in fixture directory through the
// ARBC_IMAGESEQ_FIXTURE_DIR compile definition (set per target).

#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/kind_imageseq/imageseq_content.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace arbc::imageseq::testfix {

// Facts about the checked-in fixture sequence (t/fixtures): four tiny 2x2 solid
// frames (red, green, blue, white) at the default native rate of 24/1, so the
// native frame period is 705'600'000 / 24 flicks and the clip spans
// [0, 4*period).
inline constexpr int k_frame_count = 4;
inline constexpr int k_width = 2;
inline constexpr int k_height = 2;
inline constexpr std::int64_t k_period_flicks = Time::flicks_per_second / 24; // 29'400'000

// The fixture frame paths, in native (ascending) order.
inline std::vector<FrameSource> fixture_frames() {
  namespace fs = std::filesystem;
  std::vector<std::string> paths;
  for (const fs::directory_entry& entry : fs::directory_iterator(ARBC_IMAGESEQ_FIXTURE_DIR)) {
    const std::string ext = entry.path().extension().string();
    if (ext == ".ppm" || ext == ".pgm") {
      paths.push_back(entry.path().string());
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<FrameSource> frames;
  frames.reserve(paths.size());
  for (std::string& path : paths) {
    frames.push_back(FrameSource{std::move(path)});
  }
  return frames;
}

// A fresh imageseq content over the fixtures at `native_rate` (default 24/1).
// Reverse playback is a layer time_map, not a content-rate change, so every
// test uses the native rate.
inline std::unique_ptr<ImageSeqContent> make_content(Rational native_rate = Rational{24, 1}) {
  return std::make_unique<ImageSeqContent>(fixture_frames(), native_rate, k_width, k_height);
}

} // namespace arbc::imageseq::testfix
