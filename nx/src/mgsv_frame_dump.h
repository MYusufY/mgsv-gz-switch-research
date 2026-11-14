// MGSV: Ground Zeroes - frame dumps of the presenter's guest output, for checking builds on the PC.
//
// Off unless MGSV_FRAME_DUMP_SECONDS is set (seconds between dumps). Writes MGSV_FRAME_DIR/mgsv_frame_NN.ppm
// (8-bit RGB), so two builds can be compared frame by frame without looking at the window. Not built for the Switch.

#pragma once

#if !defined(__SWITCH__)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include <rex/ui/presenter.h>

namespace mgsv {

class FrameDump {
 public:
  using PresenterGetter = std::function<rex::ui::Presenter*()>;

  void Start(PresenterGetter get_presenter) {
    const char* interval = std::getenv("MGSV_FRAME_DUMP_SECONDS");
    if (!interval || std::atoi(interval) <= 0) {
      return;
    }
    interval_s_ = std::atoi(interval);
    const char* dir = std::getenv("MGSV_FRAME_DIR");
    dir_ = dir && *dir ? dir : "/tmp";
    get_presenter_ = std::move(get_presenter);
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
  }

  void Stop() {
    stop_ = true;
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  ~FrameDump() { Stop(); }

 private:
  bool SleepTenths(int tenths) {
    for (int i = 0; i < tenths && !stop_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return !stop_;
  }

  void Loop() {
    for (int index = 0; index < 1000 && SleepTenths(interval_s_ * 10); ++index) {
      DumpOnce(index);
    }
  }

  void DumpOnce(int index) {
    rex::ui::Presenter* presenter = get_presenter_ ? get_presenter_() : nullptr;
    if (!presenter) {
      return;
    }
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image) || image.data.empty()) {
      return;
    }
    char name[512];
    std::snprintf(name, sizeof(name), "%s/mgsv_frame_%02d.ppm", dir_.c_str(), index);
    FILE* f = std::fopen(name, "wb");
    if (!f) {
      return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    std::string line(size_t(image.width) * 3, '\0');
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + size_t(y) * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        line[x * 3 + 0] = static_cast<char>(row[x * 4 + 0]);
        line[x * 3 + 1] = static_cast<char>(row[x * 4 + 1]);
        line[x * 3 + 2] = static_cast<char>(row[x * 4 + 2]);
      }
      std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
  }

  PresenterGetter get_presenter_;
  std::string dir_;
  int interval_s_ = 5;
  std::atomic<bool> stop_{true};
  std::thread thread_;
};

}  // namespace mgsv

#endif  // !__SWITCH__
