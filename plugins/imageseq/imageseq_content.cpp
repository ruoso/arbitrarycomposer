#include <arbc/base/geometry.hpp>
#include <arbc/kind_imageseq/imageseq_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <imdec.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <utility>

namespace arbc::imageseq {

namespace {

// The 128-bit intermediate the native-frame math floors/rounds through. Named
// once behind `__extension__` so the pedantic build stays clean, mirroring
// `base/rational_time.hpp`'s `rational_i128` (the frame-instant math is the same
// exact-rational, single-ties-to-even-rounding shape).
__extension__ typedef __int128 i128;

// Bounded decoded-frame cache size. Larger than any fixture sequence, so the
// tests exercise the miss-then-hit path without eviction; a real large sequence
// would evict LRU (the counter and provided-surface refcount stay correct
// because an evicted-but-still-consumed frame is retained by its SurfaceRef).
constexpr std::size_t k_max_resident = 16;

std::vector<unsigned char> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

} // namespace

// A CPU-memory working-space surface owned by the plugin (no Backend edge).
class FrameSurface final : public Surface {
public:
  FrameSurface(int width, int height)
      : d_width(width), d_height(height),
        d_data(static_cast<std::size_t>(width) * height * bytes_per_pixel(d_format.pixel_format)) {}

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return d_format; }
  std::span<std::byte> cpu_bytes() override { return d_data; }
  std::span<const std::byte> cpu_bytes() const override { return d_data; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format{k_working_rgba32f};
  std::vector<std::byte> d_data;
};

ImageSeqContent::ImageSeqContent(std::vector<FrameSource> frames, Rational native_rate, int width,
                                 int height)
    : d_frames(std::move(frames)), d_rate(native_rate), d_width(width), d_height(height) {
  d_period = instant_for_index(1);
}

ImageSeqContent::~ImageSeqContent() = default;

std::optional<Rect> ImageSeqContent::bounds() const {
  return Rect{0.0, 0.0, static_cast<double>(d_width), static_cast<double>(d_height)};
}

Stability ImageSeqContent::stability() const { return Stability::Timed; }

std::optional<TimeRange> ImageSeqContent::time_extent() const {
  // [0, N*period): never nullopt (that would be Static, doc 11:81-86).
  return TimeRange{Time::zero(), instant_for_index(frame_count())};
}

int ImageSeqContent::frame_index_for(Time t) const {
  if (d_frames.empty()) {
    return 0;
  }
  // floor(t_seconds * rate) = floor(t.flicks * num / (fps * den)); den > 0 and
  // fps > 0, so the divisor is positive and only the dividend carries a sign.
  const i128 numer = static_cast<i128>(t.flicks) * d_rate.num();
  const i128 denom = static_cast<i128>(Time::flicks_per_second) * d_rate.den();
  i128 q = numer / denom;
  if ((numer % denom) != 0 && (numer < 0)) {
    --q; // round toward negative infinity
  }
  if (q < 0) {
    q = 0;
  }
  const i128 last = frame_count() - 1;
  if (q > last) {
    q = last;
  }
  return static_cast<int>(q);
}

Time ImageSeqContent::instant_for_index(std::int64_t index) const {
  // round_ties_even(index * fps * den / num). index >= 0 and num > 0, so the
  // operands are non-negative and the tie rule is a plain even-round.
  const i128 numer = static_cast<i128>(index) * Time::flicks_per_second * d_rate.den();
  const i128 denom = d_rate.num();
  i128 q = numer / denom;
  const i128 r = numer - q * denom;
  const i128 twice = 2 * r;
  if (twice > denom || (twice == denom && (q & 1) != 0)) {
    ++q;
  }
  return Time{static_cast<std::int64_t>(q)};
}

std::optional<Time> ImageSeqContent::quantize_time(Time t) const {
  if (d_frames.empty()) {
    return std::nullopt;
  }
  return instant_for_index(frame_index_for(t));
}

ImageSeqContent::FramePtr ImageSeqContent::resolve_locked(int index) {
  for (auto it = d_cache.begin(); it != d_cache.end(); ++it) {
    if (it->first == index) {
      FramePtr frame = it->second;
      d_cache.erase(it);
      d_cache.emplace_back(index, frame); // move to MRU
      return frame;
    }
  }

  const std::vector<unsigned char> bytes =
      read_file(d_frames[static_cast<std::size_t>(index)].path);
  if (bytes.empty()) {
    return nullptr;
  }
  int w = 0;
  int h = 0;
  unsigned char* rgba = imdec_load_from_memory(bytes.data(), bytes.size(), &w, &h);
  if (rgba == nullptr || w != d_width || h != d_height) {
    imdec_free(rgba);
    return nullptr;
  }

  auto frame = std::make_shared<FrameSurface>(d_width, d_height);
  const std::span<float> dst = frame->span<PixelFormat::Rgba32fLinearPremul>();
  const std::size_t pixels = static_cast<std::size_t>(d_width) * d_height;
  for (std::size_t i = 0; i < pixels; ++i) {
    const WorkingPixel wp = PixelTraits<PixelFormat::Rgba8Srgb>::decode(&rgba[i * 4]);
    PixelTraits<PixelFormat::Rgba32fLinearPremul>::encode(wp, &dst[i * 4]);
  }
  imdec_free(rgba);

  ++d_decodes_issued;
  d_cache.emplace_back(index, frame);
  if (d_cache.size() > k_max_resident) {
    d_cache.pop_front();
  }
  return frame;
}

std::optional<RenderResult> ImageSeqContent::render(const RenderRequest& request,
                                                    std::shared_ptr<RenderCompletion> done) {
  std::lock_guard<std::mutex> lock(d_mutex);
  if (d_frames.empty()) {
    if (done) {
      done->fail(RenderError::ResourceUnavailable);
    }
    return std::nullopt;
  }

  const int index = frame_index_for(request.time);
  const FramePtr frame = resolve_locked(index);
  if (!frame) {
    // A missing/corrupt frame is an error VALUE, never UB (constraint §7).
    if (done) {
      done->fail(RenderError::ResourceUnavailable);
    }
    return std::nullopt;
  }
  d_last_index = index;

  RenderResult result;
  result.achieved_scale = request.scale;
  result.exact = true;
  // The doc-11 MUST: achieved_time == quantize_time(time) (content.hpp:261-263).
  result.achieved_time = instant_for_index(index);
  // Return the decoded frame as a non-transient refcounted provided surface
  // (Decision 4): capturing `frame` in the release callback retains it for the
  // compositor's consume window even if it is later evicted from the cache.
  result.provided.emplace(
      *frame, [frame]() { /* retained until last release */ },
      /*transient=*/false);
  return result;
}

void ImageSeqContent::playback_hint(const PlaybackHint& hint) {
  std::lock_guard<std::mutex> lock(d_mutex);
  // Advisory and correctness-neutral (constraint §6): pre-roll only warms the
  // decoded-frame cache; rendered pixels are identical with or without a hint.
  if (hint.direction == 0 || d_frames.empty()) {
    return; // an empty/paused hint pre-rolls nothing
  }
  const std::int64_t period = d_period.flicks;
  if (period <= 0) {
    return;
  }
  // Exactly the frames the temporal ring covers: K = horizon / period.
  const std::int64_t k = hint.horizon.flicks / period;
  const int anchor = d_last_index >= 0 ? d_last_index : 0;
  for (std::int64_t step = 1; step <= k; ++step) {
    const long long target =
        static_cast<long long>(anchor) + static_cast<long long>(hint.direction) * step;
    if (target < 0 || target >= frame_count()) {
      break; // bounded by the clip edges
    }
    resolve_locked(static_cast<int>(target));
  }
}

std::uint64_t ImageSeqContent::decodes_issued() const noexcept {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_decodes_issued;
}

expected<std::unique_ptr<Content>, std::string> make_imageseq_content(ContentConfig config) {
  namespace fs = std::filesystem;
  const std::string dir(config);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return unexpected<std::string>("imageseq: not a directory: " + dir);
  }

  std::vector<std::string> paths;
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const std::string ext = entry.path().extension().string();
    if (ext == ".ppm" || ext == ".pgm") {
      paths.push_back(entry.path().string());
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.empty()) {
    return unexpected<std::string>("imageseq: no frames in " + dir);
  }

  const std::vector<unsigned char> probe = read_file(paths.front());
  int w = 0;
  int h = 0;
  int channels = 0;
  if (probe.empty() || imdec_info_from_memory(probe.data(), probe.size(), &w, &h, &channels) == 0) {
    return unexpected<std::string>("imageseq: undecodable frame: " + paths.front());
  }

  std::vector<FrameSource> frames;
  frames.reserve(paths.size());
  for (std::string& path : paths) {
    frames.push_back(FrameSource{std::move(path)});
  }
  // Native rate defaults to 24/1 for the stage-1 seam (Decision 1); a
  // serialization format (doc 08) will carry the authored rate later.
  return std::unique_ptr<Content>(
      std::make_unique<ImageSeqContent>(std::move(frames), Rational{24, 1}, w, h));
}

} // namespace arbc::imageseq
