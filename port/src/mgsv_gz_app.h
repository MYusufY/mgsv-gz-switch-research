// mgsv_gz - ReXGlue Recompiled Project
//
// App hooks on top of rex::ReXApp. Two bring-up aids live here:
//
//  * Framebuffer dump. On macOS the window cannot be captured with
//    `screencapture` without screen-recording permission, and during bring-up
//    the question "is the game actually drawing, or is this a black frame?"
//    comes up constantly. Presenter::CaptureGuestOutput() is documented as
//    callable from any thread, so a small watchdog thread writes PPMs.
//
//      MGSV_FRAME_DUMP_SECONDS=10   seconds between dumps (0/unset = off)
//      MGSV_FRAME_DIR=/tmp          output directory
//      MGSV_FRAME_GAIN=64           multiply before writing (see note below)
//
//    The default xenos plugin keeps 10-bit guest output in a 16-bit UNORM
//    intermediate, which CaptureGuestOutput then narrows as if it were a plain
//    16-bit image, so UI white lands on ~4/255. That is a capture-path scaling
//    artifact, not the rendered image; gain=64 restores it for inspection.
//
//  * On-screen guest output overlay. The same 16-bit narrowing also affects the
//    presenter's own paint pass, so the host window shows an almost black image
//    even though the guest is drawing correctly. This registers a UIDrawer that
//    re-uploads CaptureGuestOutput() (gain corrected) over the guest output:
//
//      MGSV_CAPTURE_OVERLAY=1       enable the overlay
//      MGSV_OVERLAY_GAIN=64         brightness multiplier
//
//    It exists only to make host bring-up visible; the Switch presenter will do
//    the 10-bit -> display conversion in its own paint path.
//
//  * Scripted key input. The automation host has no focusable window and no
//    Screen Recording/Accessibility permission, so AppleScript keystrokes are
//    not an option. Instead we push SDL key events into the same event queue the
//    window already drains, which reaches the keyboard->pad driver unchanged.
//
//      MGSV_INPUT_SCRIPT="20000:shift+right,25000:space"
//
//    Entries are milliseconds (from process start) : key, comma separated.
//    Supported: space, return, escape, tab, backspace, left, right, up, down,
//    letters and digits, optionally prefixed with shift+.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/ui/ui_drawer.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <execinfo.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include "present_probe.h"
#endif

class MgsvGzApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MgsvGzApp>(new MgsvGzApp(ctx, "mgsv_gz",
        PPCImageConfig));
  }

  ~MgsvGzApp() override { StopThreads(); }

  void OnPostSetup() override {
    // Plugin cvars (registered when the GPU plugin loads) are not reachable
    // from the command line, which only binds flags known at cvar::Init time.
    // Dump every registered cvar with its effective value, and allow applying
    // plugin flags after registration.
    if (const char* dump_path = std::getenv("MGSV_DUMP_CVARS")) {
      if (FILE* file = std::fopen(dump_path, "wb")) {
        std::fprintf(file,
                     "# name\tcategory\tlifecycle\tdefault\tcurrent\tsource\tdescription\n");
        size_t count = 0;
        for (const rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
          std::string description = entry.description;
          for (char& c : description) {
            if (c == '\t' || c == '\n' || c == '\r') {
              c = ' ';
            }
          }
          std::fprintf(file, "%s\t%s\t%d\t%s\t%s\t%d\t%s\n", entry.name.c_str(),
                       entry.category.c_str(), static_cast<int>(entry.lifecycle),
                       entry.default_value.c_str(),
                       rex::cvar::GetFlagByName(entry.name).c_str(),
                       static_cast<int>(entry.source), description.c_str());
          ++count;
        }
        std::fclose(file);
        std::fprintf(stderr, "[cvar] wrote %zu flags to %s\n", count, dump_path);
      } else {
        std::fprintf(stderr, "[cvar] could not write %s\n", dump_path);
      }
    }
    if (const char* sets = std::getenv("MGSV_CVARS")) {
      std::string list(sets);
      size_t start = 0;
      while (start < list.size()) {
        size_t end = list.find(';', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos
                                                                    : end - start);
        size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) {
          std::string name = item.substr(0, eq);
          std::string value = item.substr(eq + 1);
          const bool ok = rex::cvar::SetFlagByName(name, value);
          std::fprintf(stderr, "[cvar] %s=%s %s (now '%s', source=%d)\n", name.c_str(),
                       value.c_str(), ok ? "ok" : "REJECTED",
                       rex::cvar::GetFlagByName(name).c_str(),
                       static_cast<int>(rex::cvar::GetFlagSource(name)));
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
    }
    if (const char* gain = std::getenv("MGSV_FRAME_GAIN")) {
      dump_gain_ = std::atoi(gain);
      if (dump_gain_ < 1) {
        dump_gain_ = 1;
      }
    }
    if (const char* interval = std::getenv("MGSV_FRAME_DUMP_SECONDS")) {
      int seconds = std::atoi(interval);
      if (seconds > 0) {
        dump_dir_ = std::getenv("MGSV_FRAME_DIR");
        if (dump_dir_.empty()) {
          dump_dir_ = "/tmp";
        }
        dump_interval_ = seconds;
        stop_ = false;
        dump_thread_ = std::thread([this] { FrameDumpLoop(); });
      }
    }
    // Raw guest-memory dump of the intermediate render targets and the final
    // frontbuffer. This answers "is the damage in the EDRAM resolve, or only in
    // the swap/present shader?" without trusting CaptureGuestOutput().
    if (const char* fb = std::getenv("MGSV_FB_DUMP")) {
      if (std::atoi(fb) != 0) {
        fb_dir_ = std::getenv("MGSV_FB_DIR");
        if (fb_dir_.empty()) {
          fb_dir_ = "/tmp";
        }
        fb_interval_ = std::atoi(std::getenv("MGSV_FB_SECONDS"));
        if (fb_interval_ <= 0) {
          fb_interval_ = 4;
        }
        fb_thread_ = std::thread([this] { GuestFbLoop(); });
      }
    }
    if (const char* script = std::getenv("MGSV_INPUT_SCRIPT")) {
      PrepareWindowForInput();
      ParseInputScript(script);
    }
#ifdef __APPLE__
    const char* present_probe_env = std::getenv("MGSV_PROBE");
    const bool present_probe =
        present_probe_env && std::atoi(present_probe_env) != 0;
    if (present_probe) {
      // The probe owns setDrawableSize:/nextDrawable/presentDrawable: and the
      // MGSV_DRAWABLE_POLICY knob, so do not install the built-in fix twice.
      MgsvPresentProbeInstall();
    } else {
      if (const char* fix = std::getenv("MGSV_FIX_DRAWABLE")) {
        g_fix_degenerate_drawable_size = std::atoi(fix) != 0;
      }
      InstallDrawableSizeFix();
    }
#endif
    if (const char* fifo = std::getenv("MGSV_INPUT_FIFO")) {
      fifo_path_ = fifo;
      PrepareWindowForInput();
      StartFifoThread();
    }
    if (const char* probe = std::getenv("MGSV_COCOA_PROBE")) {
      if (std::atoi(probe) != 0) {
#ifdef __APPLE__
#endif
        LogCocoaState("setup");
        cocoa_probe_thread_ = std::thread([this] { CocoaProbeLoop(); });
      }
    }
    if (const char* overlay = std::getenv("MGSV_CAPTURE_OVERLAY")) {
      overlay_enabled_ = std::atoi(overlay) != 0;
    }
    if (const char* gain = std::getenv("MGSV_OVERLAY_GAIN")) {
      overlay_gain_ = std::atoi(gain);
      if (overlay_gain_ < 1) {
        overlay_gain_ = 1;
      }
    }
    if (const char* every = std::getenv("MGSV_OVERLAY_CAPTURE_EVERY")) {
      overlay_capture_every_ = std::atoi(every);
      if (overlay_capture_every_ < 0) {
        overlay_capture_every_ = 0;
      }
    }
    if (const char* marker = std::getenv("MGSV_OVERLAY_MARKER")) {
      overlay_marker_ = std::atoi(marker) != 0;
    }
    if (const char* marker_size = std::getenv("MGSV_OVERLAY_MARKER_SIZE")) {
      overlay_marker_size_ = static_cast<float>(std::atof(marker_size));
      if (overlay_marker_size_ < 8.0f) {
        overlay_marker_size_ = 8.0f;
      }
    }
    if (overlay_enabled_) {
      rex::Runtime* rt = runtime();
      rex::ui::Presenter* presenter =
          rt && rt->graphics_system() ? rt->graphics_system()->presenter() : nullptr;
      if (presenter) {
        presenter->AddUIDrawerFromUIThread(&overlay_drawer_, 1000);
        std::fprintf(stderr,
                     "[overlay] guest output overlay enabled (gain=%d)\n",
                     overlay_gain_);
      } else {
        std::fprintf(stderr, "[overlay] no presenter, overlay disabled\n");
        overlay_enabled_ = false;
      }
    }
    // UI drawers are only executed by the platform paint event handler, and the
    // SDL backend delivers that event on request rather than continuously. The
    // guest thread paints guest output alone, so without this tick the capture
    // overlay (and any other UIDrawer) is executed but never reaches a
    // presented swapchain image. RequestPaint() is documented as callable from
    // non-UI threads; it just pushes an SDL event that the main loop turns into
    // OnPaint(true) on the UI thread.
    if (const char* force_ui = std::getenv("MGSV_FORCE_UI_PAINT")) {
      if (std::atoi(force_ui) != 0) {
        ui_paint_thread_ = std::thread([this] { UiPaintLoop(); });
      }
    }
    // Host-only proof that the guest is rendering: a Core Animation view fed by
    // Presenter::CaptureGuestOutput(). Independent of the presenter's UI-drawer
    // pass, which does not reach the presented swapchain on this SDL backend.
    if (const char* capture_view = std::getenv("MGSV_CAPTURE_VIEW")) {
      if (std::atoi(capture_view) != 0) {
        if (const char* gain = std::getenv("MGSV_CAPTURE_VIEW_GAIN")) {
          capture_view_gain_ = std::atoi(gain);
          if (capture_view_gain_ < 1) {
            capture_view_gain_ = 1;
          }
        }
        capture_view_thread_ = std::thread([this] { CaptureViewLoop(); });
      }
    }
  }

  void OnShutdown() override {
#ifdef __APPLE__
    MgsvPresentProbeShutdown();
#endif
    StopThreads();
  }

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    std::fprintf(stderr, "[input] app OnKeyDown vk=0x%04X shift=%d ctrl=%d\n",
                 static_cast<unsigned>(e.virtual_key()), e.is_shift_pressed() ? 1 : 0,
                 e.is_ctrl_pressed() ? 1 : 0);
  }

  void OnKeyUp(rex::ui::KeyEvent& e) override {
    std::fprintf(stderr, "[input] app OnKeyUp vk=0x%04X\n",
                 static_cast<unsigned>(e.virtual_key()));
  }

  void OnWindowFocusChanged(bool focused) override {
    std::fprintf(stderr, "[input] window focus changed -> %d\n", focused ? 1 : 0);
  }

 private:
  struct TimedKey {
    int64_t at_ms;
    SDL_Scancode scancode;
    SDL_Keycode keycode;
    bool shift;
  };

  // Presents the guest output over the (10-bit-dark) presenter image. See the
  // file header for why this exists.
  class CaptureOverlayDrawer : public rex::ui::UIDrawer {
   public:
    explicit CaptureOverlayDrawer(MgsvGzApp* app) : app_(app) {}
    void Draw(rex::ui::UIDrawContext& context) override {
      app_->DrawCaptureOverlay(context);
    }

   private:
    MgsvGzApp* app_;
  };

  void DrawCaptureOverlay(rex::ui::UIDrawContext& context) {
    if (!overlay_enabled_) {
      return;
    }
    rex::ui::ImmediateDrawer* drawer = immediate_drawer();
    rex::Runtime* rt = runtime();
    if (!drawer || !rt || !rt->graphics_system()) {
      return;
    }
    rex::ui::Presenter* presenter = rt->graphics_system()->presenter();
    if (!presenter) {
      return;
    }

    const float target_width = static_cast<float>(context.render_target_width());
    const float target_height = static_cast<float>(context.render_target_height());
    if (target_width <= 0.0f || target_height <= 0.0f) {
      return;
    }

    drawer->Begin(context, -1.0f, -1.0f);

    // Recapture only occasionally and keep the texture alive between captures:
    // capturing and uploading 1280x720 every paint is expensive, and the
    // immediate drawer allocates a new Vulkan image per CreateTexture, which
    // exhausted device memory when done at the full paint rate.
    ++overlay_frame_;
    if (overlay_frame_ % 600 == 0) {
      std::fprintf(stderr, "[overlay] draws=%d texture=%p\n", overlay_frame_,
                   static_cast<void*>(overlay_texture_.get()));
    }
    const bool capture_now =
        overlay_capture_every_ > 0 && (overlay_frame_ % overlay_capture_every_) == 0;
    if (capture_now) {
      rex::ui::RawImage image;
      if (presenter->CaptureGuestOutput(image) && !image.data.empty() &&
          image.width && image.height) {
        const uint32_t width = image.width;
        const uint32_t height = image.height;
        overlay_pixels_.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* source = image.data.data() + y * image.stride;
          uint8_t* destination = overlay_pixels_.data() + static_cast<size_t>(y) * width * 4;
          for (uint32_t x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
              int value = source[x * 4 + channel] * overlay_gain_;
              destination[x * 4 + channel] =
                  static_cast<uint8_t>(value > 255 ? 255 : value);
            }
            destination[x * 4 + 3] = 255;
          }
        }
        overlay_texture_ =
            drawer->CreateTexture(width, height, rex::ui::ImmediateTextureFilter::kLinear,
                                  false, overlay_pixels_.data());
        if (overlay_draw_count_ < 3) {
          std::fprintf(stderr, "[overlay] capture #%d %ux%u gain=%d\n",
                       overlay_draw_count_ + 1, width, height, overlay_gain_);
        }
        ++overlay_draw_count_;
      }
    }

    if (overlay_texture_) {
      rex::ui::ImmediateVertex vertices[6] = {
          {0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFF},
          {target_width, 0.0f, 1.0f, 0.0f, 0xFFFFFFFF},
          {0.0f, target_height, 0.0f, 1.0f, 0xFFFFFFFF},
          {target_width, 0.0f, 1.0f, 0.0f, 0xFFFFFFFF},
          {target_width, target_height, 1.0f, 1.0f, 0xFFFFFFFF},
          {0.0f, target_height, 0.0f, 1.0f, 0xFFFFFFFF},
      };
      rex::ui::ImmediateDrawBatch batch;
      batch.vertices = vertices;
      batch.vertex_count = 6;
      drawer->BeginDrawBatch(batch);
      rex::ui::ImmediateDraw draw;
      draw.primitive_type = rex::ui::ImmediatePrimitiveType::kTriangles;
      draw.count = 6;
      draw.texture = overlay_texture_.get();
      drawer->Draw(draw);
      drawer->EndDrawBatch();
    }

    // Animated marker: proves the overlay paint path itself works even if the
    // guest output capture is unavailable. The colour changes over time so a
    // frozen/stale window can't be mistaken for a live one.
    if (overlay_marker_) {
      const float marker = overlay_marker_size_;
      const uint32_t ticks = SDL_GetTicks();
      const uint8_t red = static_cast<uint8_t>(128 + 127 * ((ticks / 500) & 1));
      const uint8_t green = static_cast<uint8_t>(128 + 127 * ((ticks / 300) & 1));
      const uint32_t color = 0xFF000000u | (static_cast<uint32_t>(red) << 16) |
                             (static_cast<uint32_t>(green) << 8) | 0x40u;
      rex::ui::ImmediateVertex marker_vertices[6] = {
          {target_width - marker, 0.0f, 0.0f, 0.0f, color},
          {target_width, 0.0f, 0.0f, 0.0f, color},
          {target_width - marker, marker, 0.0f, 0.0f, color},
          {target_width, 0.0f, 0.0f, 0.0f, color},
          {target_width, marker, 0.0f, 0.0f, color},
          {target_width - marker, marker, 0.0f, 0.0f, color},
      };
      rex::ui::ImmediateDrawBatch batch;
      batch.vertices = marker_vertices;
      batch.vertex_count = 6;
      drawer->BeginDrawBatch(batch);
      rex::ui::ImmediateDraw draw;
      draw.primitive_type = rex::ui::ImmediatePrimitiveType::kTriangles;
      draw.count = 6;
      drawer->Draw(draw);
      drawer->EndDrawBatch();
    }

    drawer->End();
  }

  // --- Cocoa window probe ----------------------------------------------------
  //
  // The presenter's swapchain is created and presented, yet the window stays
  // black even with a solid-colour UI drawer. This dumps the state of the
  // NSWindow / CAMetalLayer that SDL created so the break in the chain is
  // visible. Enable with MGSV_COCOA_PROBE=1.

#ifdef __APPLE__
  // Layout-compatible with NSRect / NSSize / NSPoint (CGRect is 4 doubles).
  struct Rect {
    double x, y, width, height;
    void Store(double* ox, double* oy, double* ow, double* oh) const {
      *ox = x;
      *oy = y;
      *ow = width;
      *oh = height;
    }
  };
  struct Size {
    double width, height;
    void Store(double* ow, double* oh) const {
      *ow = width;
      *oh = height;
    }
  };
  struct Point {
    double x, y;
    void Store(double* ox, double* oy) const {
      *ox = x;
      *oy = y;
    }
  };

  static const char* ClassName(void* object) {
    return object ? object_getClassName(reinterpret_cast<struct objc_object*>(object)) : "null";
  }

  template <typename R, typename... Args>
  static R ObjC(void* object, const char* selector, Args... args) {
    return reinterpret_cast<R (*)(void*, SEL, Args...)>(objc_msgSend)(
        object, sel_registerName(selector), args...);
  }
#endif

  void LogCocoaState(const char* label) {
#ifdef __APPLE__
    int window_count = 0;
    SDL_Window** windows = SDL_GetWindows(&window_count);
    if (!windows || window_count == 0) {
      std::fprintf(stderr, "[cocoa] %s: no SDL windows\n", label);
      return;
    }
    SDL_Window* sdl_window = windows[0];
    SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_window);
    int width = 0, height = 0, pixel_width = 0, pixel_height = 0, x = 0, y = 0;
    SDL_GetWindowSize(sdl_window, &width, &height);
    SDL_GetWindowSizeInPixels(sdl_window, &pixel_width, &pixel_height);
    SDL_GetWindowPosition(sdl_window, &x, &y);
    std::fprintf(stderr,
                 "[cocoa] %s: flags=0x%llX size=%dx%d pixels=%dx%d pos=%d,%d "
                 "opacity=%.2f hidden=%d occluded=%d\n",
                 label, static_cast<unsigned long long>(flags), width, height,
                 pixel_width, pixel_height, x, y, SDL_GetWindowOpacity(sdl_window),
                 (flags & SDL_WINDOW_HIDDEN) ? 1 : 0,
                 (flags & SDL_WINDOW_OCCLUDED) ? 1 : 0);

    SDL_PropertiesID properties = SDL_GetWindowProperties(sdl_window);
    void* ns_window =
        SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window) {
      std::fprintf(stderr, "[cocoa] no NSWindow property\n");
      return;
    }
    std::fprintf(stderr,
                 "[cocoa] nsWindow class=%s visible=%d miniaturized=%d occlusion=0x%llX "
                 "alpha=%.2f onActiveSpace=%d screen=%p number=%lld level=%lld\n",
                 ClassName(ns_window), ObjC<unsigned char>(ns_window, "isVisible"),
                 ObjC<unsigned char>(ns_window, "isMiniaturized"),
                 static_cast<unsigned long long>(ObjC<unsigned long>(ns_window, "occlusionState")),
                 ObjC<double>(ns_window, "alphaValue"),
                 ObjC<unsigned char>(ns_window, "isOnActiveSpace"),
                 ObjC<void*>(ns_window, "screen"),
                 static_cast<long long>(ObjC<long>(ns_window, "windowNumber")),
                 static_cast<long long>(ObjC<long>(ns_window, "level")));

    void* content_view = ObjC<void*>(ns_window, "contentView");
    if (content_view) {
      std::fprintf(stderr,
                   "[cocoa] contentView class=%s hidden=%d wantsLayer=%d layer=%p\n",
                   ClassName(content_view),
                   ObjC<unsigned char>(content_view, "isHidden"),
                   ObjC<unsigned char>(content_view, "wantsLayer"),
                   ObjC<void*>(content_view, "layer"));
      void* subviews = ObjC<void*>(content_view, "subviews");
      unsigned long subview_count =
          subviews ? ObjC<unsigned long>(subviews, "count") : 0;
      std::fprintf(stderr, "[cocoa] subviews=%lu\n", subview_count);
      for (unsigned long i = 0; i < subview_count; ++i) {
        void* subview = ObjC<void*>(subviews, "objectAtIndex:", i);
        void* layer = subview ? ObjC<void*>(subview, "layer") : nullptr;
        std::fprintf(stderr,
                     "[cocoa]   sub[%lu] class=%s tag=%lld hidden=%d layer=%p",
                     i, ClassName(subview), 
                     subview ? static_cast<long long>(ObjC<long>(subview, "tag")) : -1,
                     subview ? ObjC<unsigned char>(subview, "isHidden") : 0, layer);
        if (layer) {
          std::fprintf(stderr,
                       " layerClass=%s hidden=%d opaque=%d contentsScale=%.2f device=%p "
                       "maxDrawables=%llu presentsWithTransaction=%d framebufferOnly=%d",
                       ClassName(layer), ObjC<unsigned char>(layer, "isHidden"),
                       ObjC<unsigned char>(layer, "isOpaque"),
                       ObjC<double>(layer, "contentsScale"), ObjC<void*>(layer, "device"),
                       static_cast<unsigned long long>(
                           ObjC<unsigned long>(layer, "maximumDrawableCount")),
                       ObjC<unsigned char>(layer, "presentsWithTransaction"),
                       ObjC<unsigned char>(layer, "framebufferOnly"));
        }
        std::fprintf(stderr, "\n");
        // Geometry is the thing that decides whether a layer is composited at
        // all: a layer can have a perfectly sized swapchain attached and still
        // never appear if its view or layer bounds are empty or off-window.
        double view_x = 0, view_y = 0, view_w = 0, view_h = 0;
        ObjC<Rect>(subview, "frame").Store(&view_x, &view_y, &view_w, &view_h);
        double layer_x = 0, layer_y = 0, layer_w = 0, layer_h = 0;
        ObjC<Rect>(layer, "bounds").Store(&layer_x, &layer_y, &layer_w, &layer_h);
        double drawable_w = 0, drawable_h = 0;
        ObjC<Size>(layer, "drawableSize").Store(&drawable_w, &drawable_h);
        double pos_x = 0, pos_y = 0;
        ObjC<Point>(layer, "position").Store(&pos_x, &pos_y);
        std::fprintf(stderr,
                     "[cocoa]     view frame=%.1f,%.1f %.1fx%.1f layer bounds=%.1f,%.1f "
                     "%.1fx%.1f drawableSize=%.0fx%.0f position=%.1f,%.1f "
                     "hiddenOrAncestor=%d opacity=%.2f superlayer=%p contents=%p "
                     "displaySync=%d allowsNextDrawableTimeout=%d\n",
                     view_x, view_y, view_w, view_h, layer_x, layer_y, layer_w, layer_h,
                     drawable_w, drawable_h, pos_x, pos_y,
                     ObjC<unsigned char>(subview, "isHiddenOrHasHiddenAncestor"),
                     static_cast<double>(ObjC<float>(layer, "opacity")),
                     ObjC<void*>(layer, "superlayer"), ObjC<void*>(layer, "contents"),
                     ObjC<unsigned char>(layer, "displaySyncEnabled"),
                     ObjC<unsigned char>(layer, "allowsNextDrawableTimeout"));
      }
      double cv_x = 0, cv_y = 0, cv_w = 0, cv_h = 0;
      ObjC<Rect>(content_view, "frame").Store(&cv_x, &cv_y, &cv_w, &cv_h);
      std::fprintf(stderr, "[cocoa] contentView frame=%.1f,%.1f %.1fx%.1f\n", cv_x, cv_y, cv_w,
                   cv_h);
    }
#endif
  }

  static void CocoaProbeMainThread(void* userdata) {
    static_cast<MgsvGzApp*>(userdata)->LogCocoaState("probe");
  }

  // CAMetalLayer.drawableSize is the size of the drawable the compositor shows.
  // It is being reset to 1x1 after the first window resize, so log every setter
  // call with a backtrace to find the caller.
#ifdef __APPLE__
  inline static IMP g_original_set_drawable_size = nullptr;
  inline static bool g_fix_degenerate_drawable_size = true;

  // MoltenVK bug workaround.
  //
  // MVKSwapchain::initCAMetalLayer handles a swapchain being recreated with
  // VkSwapchainCreateInfoKHR::oldSwapchain (which the presenter passes on every
  // resize) by resetting the drawable size of the CAMetalLayer that the old
  // swapchain was rendering into to 1x1, so that the old drawables get
  // released. That layer is the *same* CAMetalLayer the new swapchain just
  // configured, and the 1x1 store happens after the correct one, so the layer
  // would be left with a single-pixel drawable.
  //
  // mpv works around the same MoltenVK behaviour by dropping degenerate stores
  // instead of rewriting them, because substituting bounds * contentsScale can
  // read a stale contentsScale during a resize. So: drop the store and leave
  // the size MoltenVK already set from the swapchain extent.
  // MGSV_FIX_DRAWABLE=0 disables this.
  static void HookedSetDrawableSize(void* self, SEL, Size requested) {
    static int dropped_count = 0;
    if (g_fix_degenerate_drawable_size && requested.width <= 1.0 && requested.height <= 1.0) {
      if (dropped_count < 8) {
        ++dropped_count;
        std::fprintf(stderr,
                     "[cocoa] dropped degenerate drawableSize %.0fx%.0f (swapchain "
                     "recreation; keeping the extent-derived size)\n",
                     requested.width, requested.height);
      }
      return;
    }
    if (g_original_set_drawable_size) {
      reinterpret_cast<void (*)(void*, SEL, Size)>(g_original_set_drawable_size)(
          self, nullptr, requested);
    }
  }

  void InstallDrawableSizeFix() {
    Class layer_class = objc_getClass("CAMetalLayer");
    if (!layer_class) {
      std::fprintf(stderr, "[cocoa] no CAMetalLayer class, drawable size fix skipped\n");
      return;
    }
    Method method = class_getInstanceMethod(layer_class, sel_registerName("setDrawableSize:"));
    if (!method) {
      std::fprintf(stderr, "[cocoa] no setDrawableSize:, drawable size fix skipped\n");
      return;
    }
    g_original_set_drawable_size = method_setImplementation(method, (IMP)HookedSetDrawableSize);
    std::fprintf(stderr, "[cocoa] drawable size fix installed (enabled=%d)\n",
                 g_fix_degenerate_drawable_size ? 1 : 0);
  }
#endif

  void CocoaProbeLoop() {
    const int delays[] = {2, 5, 10, 20, 40, 80};
    int elapsed = 0;
    for (int delay : delays) {
      for (; elapsed < delay * 1000 && !stop_; elapsed += 250) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }
      if (stop_) {
        return;
      }
      SDL_RunOnMainThread(CocoaProbeMainThread, this, true);
    }
  }

  // Runs on the SDL main thread. RequestPaint() alone reaches OnPaint(false),
  // which the presenter may skip when the guest refresh thread has already
  // painted. RequestPresenterUIPaintFromUIThread() marks that a UI-inclusive
  // paint is needed, and the following RequestPaint() delivers the event that
  // performs it.
  static void ForceUIPaintOnUIThread(void* user) {
    auto* app = static_cast<MgsvGzApp*>(user);
    if (app->stop_) {
      return;
    }
    if (rex::ui::Window* w = app->window()) {
      w->RequestPresenterUIPaintFromUIThread();
      w->RequestPaint();
    }
  }

  void UiPaintLoop() {
    uint64_t ticks = 0;
    while (!stop_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      SDL_RunOnMainThread(ForceUIPaintOnUIThread, this, true);
      if (++ticks % 300 == 0) {
        std::fprintf(stderr, "[ui-paint] forced %llu UI-thread paints\n",
                     (unsigned long long)ticks);
      }
    }
  }

  static void CreateCaptureViewOnUIThread(void*) { MgsvCaptureViewCreate(); }

  void CaptureViewLoop() {
    // Give the window and presenter a moment to come up.
    for (int i = 0; i < 30 && !stop_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stop_) {
      return;
    }
    SDL_RunOnMainThread(CreateCaptureViewOnUIThread, this, true);
    std::vector<uint8_t> pixels;
    uint64_t frames = 0;
    while (!stop_) {
      rex::Runtime* rt = runtime();
      rex::ui::Presenter* presenter =
          rt && rt->graphics_system() ? rt->graphics_system()->presenter() : nullptr;
      rex::ui::RawImage image;
      if (presenter && presenter->CaptureGuestOutput(image) && !image.data.empty() &&
          image.width && image.height) {
        const uint32_t width = image.width;
        const uint32_t height = image.height;
        pixels.resize(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
          const uint8_t* source = image.data.data() + static_cast<size_t>(y) * image.stride;
          uint8_t* destination = pixels.data() + static_cast<size_t>(y) * width * 4;
          for (uint32_t x = 0; x < width; ++x) {
            for (int channel = 0; channel < 3; ++channel) {
              int value = source[x * 4 + channel] * capture_view_gain_;
              destination[x * 4 + channel] = static_cast<uint8_t>(value > 255 ? 255 : value);
            }
            destination[x * 4 + 3] = 255;
          }
        }
        MgsvCaptureViewUpdate(pixels.data(), static_cast<int>(width),
                              static_cast<int>(height), static_cast<int>(width) * 4);
        if (++frames % 100 == 0) {
          std::fprintf(stderr, "[capture-view] updated %llu frames (%ux%u gain=%d)\n",
                       (unsigned long long)frames, width, height, capture_view_gain_);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  void StopThreads() {
    stop_ = true;
    if (dump_thread_.joinable()) {
      dump_thread_.join();
    }
    if (fb_thread_.joinable()) {
      fb_thread_.join();
    }
    if (ui_paint_thread_.joinable()) {
      ui_paint_thread_.join();
    }
    if (capture_view_thread_.joinable()) {
      capture_view_thread_.join();
    }
    if (input_thread_.joinable()) {
      input_thread_.join();
    }
    if (fifo_thread_.joinable()) {
      fifo_thread_.join();
    }
    if (cocoa_probe_thread_.joinable()) {
      cocoa_probe_thread_.join();
    }
  }

  static void SleepUntil(const std::chrono::steady_clock::time_point& t,
                         const std::atomic<bool>& stop) {
    while (!stop && std::chrono::steady_clock::now() < t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  // --- Scripted input -------------------------------------------------------

  static bool LookupKey(const char* name, SDL_Scancode* scancode,
                        SDL_Keycode* keycode) {
    static const struct {
      const char* name;
      SDL_Scancode scancode;
      SDL_Keycode keycode;
    } kKeys[] = {
        {"space", SDL_SCANCODE_SPACE, SDLK_SPACE},
        {"return", SDL_SCANCODE_RETURN, SDLK_RETURN},
        {"enter", SDL_SCANCODE_RETURN, SDLK_RETURN},
        {"escape", SDL_SCANCODE_ESCAPE, SDLK_ESCAPE},
        {"tab", SDL_SCANCODE_TAB, SDLK_TAB},
        {"backspace", SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE},
        {"left", SDL_SCANCODE_LEFT, SDLK_LEFT},
        {"right", SDL_SCANCODE_RIGHT, SDLK_RIGHT},
        {"up", SDL_SCANCODE_UP, SDLK_UP},
        {"down", SDL_SCANCODE_DOWN, SDLK_DOWN},
        {"a", SDL_SCANCODE_A, SDLK_A},
        {"b", SDL_SCANCODE_B, SDLK_B},
        {"c", SDL_SCANCODE_C, SDLK_C},
        {"d", SDL_SCANCODE_D, SDLK_D},
        {"e", SDL_SCANCODE_E, SDLK_E},
        {"f", SDL_SCANCODE_F, SDLK_F},
        {"g", SDL_SCANCODE_G, SDLK_G},
        {"h", SDL_SCANCODE_H, SDLK_H},
        {"i", SDL_SCANCODE_I, SDLK_I},
        {"j", SDL_SCANCODE_J, SDLK_J},
        {"k", SDL_SCANCODE_K, SDLK_K},
        {"l", SDL_SCANCODE_L, SDLK_L},
        {"m", SDL_SCANCODE_M, SDLK_M},
        {"n", SDL_SCANCODE_N, SDLK_N},
        {"o", SDL_SCANCODE_O, SDLK_O},
        {"p", SDL_SCANCODE_P, SDLK_P},
        {"q", SDL_SCANCODE_Q, SDLK_Q},
        {"r", SDL_SCANCODE_R, SDLK_R},
        {"s", SDL_SCANCODE_S, SDLK_S},
        {"t", SDL_SCANCODE_T, SDLK_T},
        {"u", SDL_SCANCODE_U, SDLK_U},
        {"v", SDL_SCANCODE_V, SDLK_V},
        {"w", SDL_SCANCODE_W, SDLK_W},
        {"x", SDL_SCANCODE_X, SDLK_X},
        {"y", SDL_SCANCODE_Y, SDLK_Y},
        {"z", SDL_SCANCODE_Z, SDLK_Z},
        {"0", SDL_SCANCODE_0, SDLK_0},
        {"1", SDL_SCANCODE_1, SDLK_1},
        {"2", SDL_SCANCODE_2, SDLK_2},
        {"3", SDL_SCANCODE_3, SDLK_3},
        {"4", SDL_SCANCODE_4, SDLK_4},
        {"5", SDL_SCANCODE_5, SDLK_5},
        {"6", SDL_SCANCODE_6, SDLK_6},
        {"7", SDL_SCANCODE_7, SDLK_7},
        {"8", SDL_SCANCODE_8, SDLK_8},
        {"9", SDL_SCANCODE_9, SDLK_9},
    };
    for (const auto& k : kKeys) {
      if (std::strcmp(k.name, name) == 0) {
        *scancode = k.scancode;
        *keycode = k.keycode;
        return true;
      }
    }
    return false;
  }

  void ParseInputScript(const char* script) {
    std::string text(script);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t comma = text.find(',', pos);
      std::string item = text.substr(
          pos, comma == std::string::npos ? std::string::npos : comma - pos);
      pos = comma == std::string::npos ? text.size() : comma + 1;

      size_t colon = item.find(':');
      if (colon == std::string::npos) {
        std::fprintf(stderr, "[input] bad entry '%s'\n", item.c_str());
        continue;
      }
      TimedKey tk{};
      tk.at_ms = std::atoll(item.substr(0, colon).c_str());
      std::string key = item.substr(colon + 1);
      if (key.rfind("shift+", 0) == 0) {
        tk.shift = true;
        key = key.substr(6);
      }
      if (!LookupKey(key.c_str(), &tk.scancode, &tk.keycode)) {
        std::fprintf(stderr, "[input] unknown key '%s'\n", key.c_str());
        continue;
      }
      if (!script_.empty() && tk.at_ms < script_.back().at_ms) {
        std::fprintf(stderr, "[input] script not ordered at '%s'\n",
                     item.c_str());
      }
      script_.push_back(tk);
    }
    if (script_.empty()) {
      return;
    }
    std::fprintf(stderr, "[input] %zu scheduled key(s)\n", script_.size());
    input_thread_ = std::thread([this] { InputLoop(); });
  }

  static void PushKey(SDL_WindowID window_id, SDL_Scancode scancode,
                      SDL_Keycode keycode, bool down, bool shift) {
    SDL_Event ev{};
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.windowID = window_id;
    ev.key.which = 0;
    ev.key.down = down;
    ev.key.repeat = false;
    ev.key.scancode = scancode;
    ev.key.key = keycode;
    ev.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
    if (!SDL_PushEvent(&ev)) {
      std::fprintf(stderr, "[input] SDL_PushEvent failed: %s\n",
                   SDL_GetError());
    }
  }

  // SDL key events are attributed to a window, and the input driver only
  // reports a connected pad while its window has focus, so both the real
  // window id and a synthetic focus event are needed: this process runs
  // without a foreground window on the automation host.
  static SDL_WindowID DiscoverWindowId() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    std::fprintf(stderr, "[input] SDL windows: %d\n", count);
    SDL_WindowID id = 0;
    for (int i = 0; i < count; ++i) {
      SDL_WindowID wid = SDL_GetWindowID(windows[i]);
      std::fprintf(stderr, "[input]   window %d id=%u title='%s'\n", i,
                   static_cast<unsigned>(wid),
                   SDL_GetWindowTitle(windows[i]));
      if (!id) {
        id = wid;
      }
    }
    return id;
  }

  // SDL video functions must run on the thread that owns SDL (the UI thread);
  // calling them from the input thread deadlocks. OnPostSetup runs on that
  // thread, after the window exists and before the message loop starts.
  void PrepareWindowForInput() {
    if (window_id_) {
      return;
    }
    window_id_ = DiscoverWindowId();
    if (window()) {
      window()->Focus();
    }
    SDL_Window* focus = SDL_GetKeyboardFocus();
    std::fprintf(stderr, "[input] keyboard focus: %s\n",
                 focus ? SDL_GetWindowTitle(focus) : "(null)");
    if (window()) {
      std::fprintf(stderr, "[input] rex window HasFocus: %d\n",
                   window()->HasFocus() ? 1 : 0);
    }
  }

  // Bring-up diagnostic: what the guest would receive for each user right now.
  void ProbeInput(const char* label) {
    rex::Runtime* rt = runtime();
    if (!rt || !rt->input_system()) {
      std::fprintf(stderr, "[input] probe(%s): no input system\n", label);
      return;
    }
    auto* sys = static_cast<rex::input::InputSystem*>(rt->input_system());
    for (uint32_t user = 0; user < 4; ++user) {
      rex::input::X_INPUT_CAPABILITIES caps{};
      rex::X_RESULT caps_result =
          sys->GetCapabilities(user, rex::input::X_INPUT_FLAG_GAMEPAD, &caps);
      rex::input::X_INPUT_STATE state{};
      rex::X_RESULT state_result = sys->GetState(user, &state);
      std::fprintf(stderr,
                   "[input] probe(%s) user%u: caps=%08X type=%u state=%08X "
                   "buttons=%04X lt=%u rt=%u\n",
                   label, user, caps_result, caps.type, state_result,
                   static_cast<uint16_t>(state.gamepad.buttons),
                   static_cast<uint8_t>(state.gamepad.left_trigger),
                   static_cast<uint8_t>(state.gamepad.right_trigger));
    }
  }

  // The OS never gives this process foreground focus, and SDL reports a focus
  // loss shortly after every synthetic gain, so focus must be re-asserted right
  // before each key: the MnK driver ignores keys while unfocused.
  void AssertFocus() {
    if (!window_id_) {
      return;
    }
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    focus.window.timestamp = SDL_GetTicks();
    focus.window.windowID = window_id_;
    SDL_PushEvent(&focus);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }

  void PressKey(SDL_Scancode scancode, SDL_Keycode keycode, bool shift) {
    AssertFocus();
    if (shift) {
      PushKey(window_id_, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, true, true);
    }
    PushKey(window_id_, scancode, keycode, true, shift);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ProbeInput("key held");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    PushKey(window_id_, scancode, keycode, false, shift);
    if (shift) {
      PushKey(window_id_, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, false, false);
    }
  }

  // "shift+right", "space", "space*3" -> injected key presses.
  void InjectKeyString(std::string spec) {
    int repeat = 1;
    if (size_t star = spec.find('*'); star != std::string::npos) {
      repeat = std::atoi(spec.c_str() + star + 1);
      spec = spec.substr(0, star);
      if (repeat < 1) {
        repeat = 1;
      }
    }
    bool shift = false;
    if (spec.rfind("shift+", 0) == 0) {
      shift = true;
      spec = spec.substr(6);
    }
    SDL_Scancode scancode;
    SDL_Keycode keycode;
    if (!LookupKey(spec.c_str(), &scancode, &keycode)) {
      std::fprintf(stderr, "[input] unknown key '%s'\n", spec.c_str());
      return;
    }
    for (int i = 0; i < repeat && !stop_; ++i) {
      std::fprintf(stderr, "[input] inject %s%s%s\n", shift ? "shift+" : "",
                   spec.c_str(), repeat > 1 ? " (repeat)" : "");
      PressKey(scancode, keycode, shift);
      if (i + 1 < repeat) {
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
      }
    }
  }

  void InputLoop() {
    ProbeInput("start");
    AssertFocus();
    const auto start = std::chrono::steady_clock::now();
    for (const TimedKey& tk : script_) {
      SleepUntil(start + std::chrono::milliseconds(tk.at_ms), stop_);
      if (stop_) {
        return;
      }
      std::fprintf(stderr, "[input] t=%lldms key %s%s\n",
                   static_cast<long long>(tk.at_ms),
                   tk.shift ? "shift+" : "", SDL_GetScancodeName(tk.scancode));
      PressKey(tk.scancode, tk.keycode, tk.shift);
    }
  }

  // --- Live input (named pipe) ----------------------------------------------
  //
  // Replaying the whole boot to try a single keypress is slow, so the app can
  // also accept keys over a FIFO:  echo "space" > /tmp/mgsv-in
  //
  //   MGSV_INPUT_FIFO=/tmp/mgsv-in
  //
  // Each line is one key spec (see InjectKeyString). Lines are processed in
  // order on a dedicated thread; the FIFO is opened read-write so writers can
  // come and go without the reader seeing EOF.

  void StartFifoThread() {
    if (::mkfifo(fifo_path_.c_str(), 0600) != 0 && errno != EEXIST) {
      std::fprintf(stderr, "[input] mkfifo(%s) failed: %s\n", fifo_path_.c_str(),
                   std::strerror(errno));
      return;
    }
    fifo_thread_ = std::thread([this] { FifoLoop(); });
  }

  void FifoLoop() {
    int fd = ::open(fifo_path_.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0) {
      std::fprintf(stderr, "[input] open(%s) failed: %s\n", fifo_path_.c_str(),
                   std::strerror(errno));
      return;
    }
    std::fprintf(stderr, "[input] listening on %s\n", fifo_path_.c_str());
    std::string pending;
    char buffer[512];
    while (!stop_) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 100) > 0 && (p.revents & POLLIN)) {
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
          pending.append(buffer, static_cast<size_t>(n));
          size_t newline;
          while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
              line.pop_back();
            }
            if (!line.empty()) {
              InjectKeyString(line);
            }
          }
        }
      }
    }
    ::close(fd);
  }

  // --- Framebuffer dump -----------------------------------------------------

  void FrameDumpLoop() {
    // Give the guest a moment to produce its first real frame.
    for (int i = 0; i < 50 && !stop_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (int index = 0; index < 600 && !stop_; ++index) {
      for (int i = 0; i < dump_interval_ * 10 && !stop_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stop_) {
        break;
      }
      DumpOnce(index);
    }
  }

  void DumpOnce(int index) {
    // Window::presenter() is protected; IGraphicsSystem exposes the same
    // pointer publicly, and it is valid for as long as the runtime lives.
    rex::Runtime* rt = runtime();
    if (!rt || !rt->graphics_system()) {
      return;
    }
    rex::ui::Presenter* presenter = rt->graphics_system()->presenter();
    if (!presenter) {
      return;
    }
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image) || image.data.empty()) {
      return;
    }

    char name[256];
    std::snprintf(name, sizeof(name), "%s/mgsv_frame_%02d.ppm",
                  dump_dir_.c_str(), index);
    FILE* f = std::fopen(name, "wb");
    if (!f) {
      return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    uint8_t line[3 * 4096];
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* row = image.data.data() + y * image.stride;
      uint32_t n = 0;
      for (uint32_t x = 0; x < image.width; ++x) {
        for (int c = 0; c < 3; ++c) {
          int v = row[x * 4 + c] * dump_gain_;
          line[n++] = static_cast<uint8_t>(v > 255 ? 255 : v);
        }
      }
      std::fwrite(line, 1, n, f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[frames] wrote %s (%ux%u)\n", name, image.width,
                 image.height);
  }

  // --- Guest memory dump ---------------------------------------------------
  //
  // The resolve addresses below are the ones the xenos plugin logs at
  // `log_level=trace` for this title (see XELOG_GPU RESOLVE lines); they are
  // stable across runs because the guest allocates its render targets
  // deterministically. Everything is guest *physical* memory and is stored
  // big-endian, but for a histogram / untiling pass the byte order does not
  // matter yet.
  struct GuestRegion {
    const char* name;
    uint32_t address;
    uint32_t size;
  };

  void GuestFbLoop() {
    for (int i = 0; i < 200 && !stop_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (int index = 0; index < 600 && !stop_; ++index) {
      if (stop_) {
        break;
      }
      DumpGuestRegions(index);
      for (int i = 0; i < fb_interval_ * 10 && !stop_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  void ScanGuestPhysical(rex::memory::Memory& mem) {
    constexpr uint32_t kChunk = 1u << 20;   // 1 MB
    constexpr uint32_t kTotal = 0x20000000u;  // 512 MB guest physical
    const uint8_t* base = mem.physical_membase();
    if (!base) {
      std::fprintf(stderr, "[fb] no physical membase\n");
      return;
    }
    std::fprintf(stderr, "[fb] scanning physical memory at %p\n",
                 static_cast<const void*>(base));
    uint64_t nonzero_total = 0;
    uint64_t chunks_with_data = 0;
    for (uint32_t off = 0; off < kTotal; off += kChunk) {
      const uint8_t* p = base + off;
      uint32_t nz = 0;
      uint8_t minv = 255, maxv = 0;
      for (uint32_t i = 0; i < kChunk; ++i) {
        const uint8_t v = p[i];
        if (v) {
          ++nz;
          if (v < minv) minv = v;
          if (v > maxv) maxv = v;
        }
      }
      if (!nz) {
        continue;
      }
      ++chunks_with_data;
      nonzero_total += nz;
      if (nz > 1024) {
        std::fprintf(stderr,
                     "[fb]  phys 0x%08X nonzero=%u range=%u..%u first=%02X%02X%02X%02X\n",
                     off, nz, minv, maxv, p[0], p[1], p[2], p[3]);
      }
    }
    std::fprintf(stderr, "[fb] physical scan: %llu chunks, %llu nonzero bytes\n",
                 static_cast<unsigned long long>(chunks_with_data),
                 static_cast<unsigned long long>(nonzero_total));
  }

  void DumpGuestRegions(int index) {
    rex::Runtime* rt = runtime();
    if (!rt || !rt->memory()) {
      return;
    }
    rex::memory::Memory* mem = rt->memory();
    static const GuestRegion regions[] = {
        // Final 1280x720 8-bit frontbuffer, double buffered.
        {"fb0", 0x1DC05000, 1280u * 720u * 4u},
        {"fb1", 0x1D86D000, 1280u * 720u * 4u},
        // 984x720 10-bit scene colour, double buffered in EDRAM resolve.
        {"scene0", 0x1B683000, 984u * 720u * 4u},
        {"scene1", 0x1B3BA000, 984u * 720u * 4u},
        // 984x720 8-bit intermediate.
        {"mid", 0x1CF79000, 984u * 720u * 4u},
        // 496x360 8-bit intermediate.
        {"small", 0x1B2F0000, 496u * 360u * 4u},
        // 496x360 16F scene colour (2_10_10_10_FLOAT resolve).
        {"hdr", 0x1B94C000, 496u * 360u * 8u},
    };
    if (!fb_dumped_) {
      std::fprintf(stderr, "[fb] physical_membase=%p virtual_membase=%p\n",
                   static_cast<void*>(mem->physical_membase()),
                   static_cast<void*>(mem->virtual_membase()));
      fb_dumped_ = true;
      ScanGuestPhysical(*mem);
    }
    for (const GuestRegion& region : regions) {
      const uint8_t* data = mem->TranslatePhysical(region.address);
      if (!data) {
        continue;
      }
      char base[256];
      std::snprintf(base, sizeof(base), "%s/mgsv_%s_%02d", fb_dir_.c_str(),
                    region.name, index);
      std::string raw_path = std::string(base) + ".bin";
      if (FILE* f = std::fopen(raw_path.c_str(), "wb")) {
        std::fwrite(data, 1, region.size, f);
        std::fclose(f);
      }
      // Order-independent 256-bin histogram: a real 3D frame spans a wide
      // range, while a clear colour + flat UI is a handful of values.
      uint64_t bins[256] = {0};
      uint64_t sum = 0;
      for (uint32_t i = 0; i < region.size; ++i) {
        ++bins[data[i]];
        sum += data[i];
      }
      int distinct = 0;
      int highest = 0;
      for (int v = 0; v < 256; ++v) {
        if (bins[v]) {
          ++distinct;
          highest = v;
        }
      }
      char hist_path[256];
      std::snprintf(hist_path, sizeof(hist_path), "%s.hist", base);
      if (FILE* hf = std::fopen(hist_path, "wb")) {
        for (int v = 0; v < 256; ++v) {
          if (bins[v]) {
            std::fprintf(hf, "%d\t%llu\n", v,
                         static_cast<unsigned long long>(bins[v]));
          }
        }
        std::fclose(hf);
      }
      std::fprintf(stderr,
                   "[fb] %s #%02d addr=0x%08X size=%u distinct=%d max=%d "
                   "mean=%.3f",
                   region.name, index, region.address, region.size, distinct,
                   highest, double(sum) / double(region.size));
      // Cross-check the virtual translation too, in case the GPU plugin maps
      // physical allocations through the virtual heap base on this platform.
      const uint8_t* vdata = mem->TranslateVirtual(region.address);
      if (vdata) {
        uint32_t vnz = 0;
        uint8_t vmax = 0;
        for (uint32_t i = 0; i < region.size; ++i) {
          if (vdata[i]) {
            ++vnz;
            if (vdata[i] > vmax) vmax = vdata[i];
          }
        }
        std::fprintf(stderr, " vnonzero=%u vmax=%u", vnz, vmax);
      }
      std::fprintf(stderr, "\n");
    }
  }

  std::thread dump_thread_;
  std::thread input_thread_;
  std::thread fifo_thread_;
  std::thread cocoa_probe_thread_;
  std::thread ui_paint_thread_;
  std::thread capture_view_thread_;
  std::thread fb_thread_;
  std::atomic<bool> stop_{false};
  std::string dump_dir_;
  std::string fb_dir_;
  bool fb_dumped_ = false;
  int fb_interval_ = 4;
  std::string fifo_path_;
  int dump_interval_ = 10;
  int dump_gain_ = 1;
  SDL_WindowID window_id_ = 0;
  std::vector<TimedKey> script_;

  CaptureOverlayDrawer overlay_drawer_{this};
  std::vector<uint8_t> overlay_pixels_;
  std::unique_ptr<rex::ui::ImmediateTexture> overlay_texture_;
  bool overlay_enabled_ = false;
  bool overlay_marker_ = true;
  int overlay_gain_ = 64;
  int overlay_capture_every_ = 10;
  int overlay_draw_count_ = 0;
  int overlay_frame_ = 0;
  int capture_view_gain_ = 64;
  float overlay_marker_size_ = 160.0f;
};
