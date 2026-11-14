// present_probe.mm -- black-window bring-up probe for Vulkan -> MoltenVK -> CAMetalLayer.
//
// NOT COMPILED OR RUN by its author (written on Linux, no macOS/Metal toolchain available).
// Expect to fix small typos on first build. Everything is env-gated and off by default.
//
//   MGSV_PROBE=1               master switch (hooks installed from a static constructor, i.e.
//                              before the first swapchain exists, so the full history is logged)
//   MGSV_DRAWABLE_POLICY=      what to do with a degenerate (<=1x1) -[CAMetalLayer setDrawableSize:]
//        ignore (default)        drop it, like mpv's MetalLayer override
//        substitute              rewrite to bounds * contentsScale (what mgsv_gz_app.h does today)
//        off                     pass it through (reproduces the original bug)
//   MGSV_PROBE_LOG_EVERY=120   log layer/drawable state on every Nth nextDrawable (first 3 always)
//   MGSV_READBACK=60           every Nth presentDrawable:, read the presented texture back and log
//                              pixel statistics (forces layer.framebufferOnly = NO)
//   MGSV_COMPOSITOR_TEST=1     adds a plain magenta NSView and a second, raw-Metal CAMetalLayer
//                              (flashing cyan/yellow) on top of the window, bottom-left/right
//
// Integration (see the reply for the CMake part):
//   #include "present_probe.h"
//   in MgsvGzApp::OnPostSetup():   MgsvPresentProbeInstall();   // and drop InstallDrawableSizeFix()
//   in MgsvGzApp::OnShutdown():    MgsvPresentProbeShutdown();
//
// Build with -fobjc-arc.

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "present_probe.h"

namespace {

enum class DrawablePolicy { kOff, kIgnore, kSubstitute };

DrawablePolicy g_policy = DrawablePolicy::kIgnore;
int g_log_every = 120;
int g_readback_every = 0;
bool g_enabled = false;

IMP g_orig_set_drawable_size = nullptr;
IMP g_orig_next_drawable = nullptr;
IMP g_orig_set_fb_only = nullptr;
IMP g_orig_present_drawable = nullptr;
IMP g_orig_present_at_time = nullptr;
IMP g_orig_present_after_duration = nullptr;
IMP g_orig_drawable_present = nullptr;
IMP g_orig_drawable_present_at_time = nullptr;

std::atomic<uint64_t> g_set_size_calls{0};
std::atomic<uint64_t> g_degenerate_calls{0};
std::atomic<uint64_t> g_next_calls{0};
std::atomic<uint64_t> g_next_nil{0};
std::atomic<uint64_t> g_present_calls{0};
std::atomic<uint64_t> g_present_at_time_calls{0};
std::atomic<uint64_t> g_present_after_calls{0};
std::atomic<uint64_t> g_drawable_present_calls{0};
std::atomic<uint64_t> g_drawable_present_at_time_calls{0};
std::atomic<bool> g_drawable_hooks_installed{false};
bool g_force_drawable_present = false;
std::atomic<bool> g_sample_inflight{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_layer_hooks_installed{false};
std::atomic<bool> g_stop{false};

__weak CAMetalLayer* g_test_layer = nil;  // our own raw-Metal layer, excluded from sampling
id<MTLCommandQueue> g_probe_queue = nil;
std::thread g_test_thread;

int EnvInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return v ? std::atoi(v) : fallback;
}

std::string ColorspaceName(CGColorSpaceRef cs) {
  if (!cs) return "(nil)";
  CFStringRef name = CGColorSpaceCopyName(cs);
  if (!name) return "(unnamed)";
  char buf[128] = {};
  CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
  CFRelease(name);
  return buf;
}

// --- -[CAMetalLayer setDrawableSize:] ---------------------------------------------------------

void HookSetDrawableSize(id self, SEL cmd, CGSize requested) {
  const uint64_t n = ++g_set_size_calls;
  CAMetalLayer* layer = (CAMetalLayer*)self;
  const CGSize current = layer.drawableSize;
  const bool degenerate = requested.width <= 1.0 && requested.height <= 1.0;
  CGSize applied = requested;
  bool call_original = true;
  if (degenerate) {
    ++g_degenerate_calls;
    if (g_policy == DrawablePolicy::kIgnore) {
      call_original = false;
    } else if (g_policy == DrawablePolicy::kSubstitute) {
      const CGRect b = layer.bounds;
      double scale = layer.contentsScale;
      if (!(scale > 0.0)) scale = 1.0;
      if (b.size.width > 1.0 && b.size.height > 1.0) {
        applied = CGSizeMake(b.size.width * scale, b.size.height * scale);
      }
    }
  }
  if (n <= 40 || (degenerate && g_degenerate_calls <= 40)) {
    std::fprintf(stderr,
                 "[probe] setDrawableSize #%llu requested=%.0fx%.0f current=%.0fx%.0f -> %s "
                 "%.0fx%.0f (policy=%d, main_thread=%d)\n",
                 (unsigned long long)n, requested.width, requested.height, current.width,
                 current.height, call_original ? "applied" : "DROPPED", applied.width,
                 applied.height, (int)g_policy, [NSThread isMainThread] ? 1 : 0);
  }
  if (call_original) {
    reinterpret_cast<void (*)(id, SEL, CGSize)>(g_orig_set_drawable_size)(self, cmd, applied);
  }
}

// --- -[CAMetalLayer setFramebufferOnly:] (readback needs a sampleable texture) ----------------

void HookSetFramebufferOnly(id self, SEL cmd, BOOL value) {
  reinterpret_cast<void (*)(id, SEL, BOOL)>(g_orig_set_fb_only)(
      self, cmd, g_readback_every > 0 ? NO : value);
}

// --- -[CAMetalLayer nextDrawable]: is the drawable texture the size/format we think? ----------

void SampleDrawable(id<MTLCommandQueue> queue, id<CAMetalDrawable> d, uint64_t n);
IMP Swizzle(Class cls, const char* selector, IMP replacement);
void InstallDrawableHooks(id drawable);

id HookNextDrawable(id self, SEL cmd) {
  id<CAMetalDrawable> d =
      reinterpret_cast<id (*)(id, SEL)>(g_orig_next_drawable)(self, cmd);
  const uint64_t n = ++g_next_calls;
  if (!d) {
    if (++g_next_nil <= 10) {
      std::fprintf(stderr, "[probe] nextDrawable returned nil (#%llu, timeout?)\n",
                   (unsigned long long)g_next_nil.load());
    }
    return d;
  }
  InstallDrawableHooks(d);
  const uint64_t presents_seen = g_present_calls.load() + g_present_at_time_calls.load() +
                                 g_present_after_calls.load();
  if (g_readback_every > 0 && presents_seen == 0 && n > 240 && n % g_readback_every == 0 &&
      (CAMetalLayer*)self != g_test_layer && !g_sample_inflight.exchange(true)) {
    if (!g_probe_queue) g_probe_queue = [((CAMetalLayer*)self).device newCommandQueue];
    std::fprintf(stderr,
                 "[probe] no present selector seen yet; sampling acquired drawable "
                 "(holds the previously presented frame)\n");
    SampleDrawable(g_probe_queue, d, n);
  }
  if (n <= 3 || (g_log_every > 0 && n % g_log_every == 0)) {
    CAMetalLayer* l = (CAMetalLayer*)self;
    id<MTLTexture> t = d.texture;
    std::fprintf(
        stderr,
        "[probe] nextDrawable #%llu layer=%p{drawableSize=%.0fx%.0f bounds=%.0fx%.0f scale=%.1f "
        "pixelFormat=%lu colorspace=%s edr=%d opaque=%d fbOnly=%d displaySync=%d} "
        "texture{%lux%lu fmt=%lu storage=%lu usage=0x%lx fbOnly=%d} "
        "drawablePresent=%llu drawablePresentAtTime=%llu\n",
        (unsigned long long)n, (__bridge void*)l, l.drawableSize.width, l.drawableSize.height, l.bounds.size.width,
        l.bounds.size.height, l.contentsScale, (unsigned long)l.pixelFormat,
        ColorspaceName(l.colorspace).c_str(), l.wantsExtendedDynamicRangeContent ? 1 : 0,
        l.isOpaque ? 1 : 0, l.framebufferOnly ? 1 : 0, l.displaySyncEnabled ? 1 : 0,
        (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat,
        (unsigned long)t.storageMode, (unsigned long)t.usage, t.framebufferOnly ? 1 : 0,
        (unsigned long long)g_drawable_present_calls.load(),
        (unsigned long long)g_drawable_present_at_time_calls.load());
  }
  return d;
}

// --- pixel readback of the texture that is actually presented ---------------------------------

float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp((float)m, -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp((float)(1024 + m), (int)e - 25);
  return s ? -v : v;
}

size_t BytesPerPixel(MTLPixelFormat f) {
  switch (f) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGR10A2Unorm:
    case MTLPixelFormatRGB10A2Unorm:
      return 4;
    case MTLPixelFormatRGBA16Float:
      return 8;
    default:
      return 0;
  }
}

void DecodePixel(const uint8_t* p, MTLPixelFormat f, float rgb[3]) {
  switch (f) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
      rgb[0] = p[2] / 255.f; rgb[1] = p[1] / 255.f; rgb[2] = p[0] / 255.f; break;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
      rgb[0] = p[0] / 255.f; rgb[1] = p[1] / 255.f; rgb[2] = p[2] / 255.f; break;
    case MTLPixelFormatBGR10A2Unorm: {
      uint32_t v; std::memcpy(&v, p, 4);
      rgb[2] = (v & 0x3FF) / 1023.f; rgb[1] = ((v >> 10) & 0x3FF) / 1023.f;
      rgb[0] = ((v >> 20) & 0x3FF) / 1023.f; break;
    }
    case MTLPixelFormatRGB10A2Unorm: {
      uint32_t v; std::memcpy(&v, p, 4);
      rgb[0] = (v & 0x3FF) / 1023.f; rgb[1] = ((v >> 10) & 0x3FF) / 1023.f;
      rgb[2] = ((v >> 20) & 0x3FF) / 1023.f; break;
    }
    case MTLPixelFormatRGBA16Float: {
      uint16_t h[4]; std::memcpy(h, p, 8);
      for (int i = 0; i < 3; ++i) rgb[i] = HalfToFloat(h[i]);
      break;
    }
    default: rgb[0] = rgb[1] = rgb[2] = 0.f;
  }
}

void Analyze(const uint8_t* data, size_t w, size_t h, size_t bpr, MTLPixelFormat f, uint64_t n) {
  const size_t bpp = BytesPerPixel(f);
  auto at = [&](size_t x, size_t y, float rgb[3]) {
    DecodePixel(data + y * bpr + x * bpp, f, rgb);
  };
  size_t total = 0, nonzero = 0;
  double sum = 0.0;
  float maxv = 0.f;
  for (int gy = 0; gy < 64; ++gy) {
    for (int gx = 0; gx < 64; ++gx) {
      float c[3];
      at((gx * w) / 64 + w / 128, (gy * h) / 64 + h / 128, c);
      ++total;
      const float m = std::fmax(c[0], std::fmax(c[1], c[2]));
      if (m > 0.f) ++nonzero;
      maxv = std::fmax(maxv, m);
      sum += (c[0] + c[1] + c[2]) / 3.0;
    }
  }
  float center[3], marker[3], corner[3];
  at(w / 2, h / 2, center);
  at(w > 80 ? w - 80 : 0, 80 < h ? 80 : 0, marker);  // centre of the 160px top-right marker
  at(40 < w ? 40 : 0, h > 40 ? h - 40 : 0, corner);
  std::fprintf(stderr,
               "[probe] READBACK present#%llu %zux%zu fmt=%lu  nonzero=%.1f%% max=%.4f mean=%.5f  "
               "center=(%.3f,%.3f,%.3f) marker=(%.3f,%.3f,%.3f) bottomleft=(%.3f,%.3f,%.3f)\n",
               (unsigned long long)n, w, h, (unsigned long)f, 100.0 * nonzero / total, maxv,
               sum / total, center[0], center[1], center[2], marker[0], marker[1], marker[2],
               corner[0], corner[1], corner[2]);
}

void SampleDrawable(id<MTLCommandQueue> queue, id<CAMetalDrawable> d, uint64_t n) {
  id<MTLTexture> t = d.texture;
  const size_t w = t.width, h = t.height, bpp = BytesPerPixel(t.pixelFormat);
  if (!bpp || t.framebufferOnly) {
    std::fprintf(stderr, "[probe] READBACK skipped: fmt=%lu fbOnly=%d (unsupported format or "
                         "layer.framebufferOnly was set before the hook; see notes)\n",
                 (unsigned long)t.pixelFormat, t.framebufferOnly ? 1 : 0);
    g_sample_inflight = false;
    return;
  }
  const size_t bpr = w * bpp;
  if (t.storageMode == MTLStorageModePrivate) {
    id<MTLBuffer> buf = [t.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
        destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [blit endEncoding];
    const MTLPixelFormat f = t.pixelFormat;
    [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
      Analyze((const uint8_t*)buf.contents, w, h, bpr, f, n);
      (void)d;  // keep the drawable (and its texture) alive until the copy is done
      g_sample_inflight = false;
    }];
    [cb commit];
  } else {
    std::vector<uint8_t> px(bpr * h);
    [t getBytes:px.data() bytesPerRow:bpr fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    Analyze(px.data(), w, h, bpr, t.pixelFormat, n);
    g_sample_inflight = false;
  }
}

void MaybeSamplePresent(id self, id<MTLDrawable> drawable, uint64_t n) {
  id<CAMetalDrawable> d = (id<CAMetalDrawable>)drawable;
  if (g_readback_every > 0 && d && d.layer != g_test_layer && n % g_readback_every == 0 &&
      !g_sample_inflight.exchange(true)) {
    id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)self;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
      SampleDrawable(done.commandQueue, d, n);
    }];
  }
}

void HookPresentDrawable(id self, SEL cmd, id<MTLDrawable> drawable) {
  const uint64_t n = ++g_present_calls;
  MaybeSamplePresent(self, drawable, n);
  reinterpret_cast<void (*)(id, SEL, id)>(g_orig_present_drawable)(self, cmd, drawable);
}

void HookPresentDrawableAtTime(id self, SEL cmd, id<MTLDrawable> drawable,
                               CFTimeInterval time) {
  const uint64_t n = ++g_present_at_time_calls;
  MaybeSamplePresent(self, drawable, n);
  reinterpret_cast<void (*)(id, SEL, id, CFTimeInterval)>(g_orig_present_at_time)(
      self, cmd, drawable, time);
}

void HookPresentDrawableAfterMinimumDuration(id self, SEL cmd, id<MTLDrawable> drawable,
                                             CFTimeInterval duration) {
  const uint64_t n = ++g_present_after_calls;
  MaybeSamplePresent(self, drawable, n);
  reinterpret_cast<void (*)(id, SEL, id, CFTimeInterval)>(g_orig_present_after_duration)(
      self, cmd, drawable, duration);
}

// --- hook installation ----------------------------------------------------------------------

IMP Swizzle(Class cls, const char* selector, IMP replacement) {
  Method m = class_getInstanceMethod(cls, sel_registerName(selector));
  if (!m) {
    std::fprintf(stderr, "[probe] no method %s on %s\n", selector, class_getName(cls));
    return nullptr;
  }
  return method_setImplementation(m, replacement);
}

// MoltenVK in this SDK presents the drawable directly ([drawable present] /
// presentAtTime:), not through a command-buffer presentDrawable: selector.
void HookDrawablePresent(id self, SEL cmd) {
  ++g_drawable_present_calls;
  reinterpret_cast<void (*)(id, SEL)>(g_orig_drawable_present)(self, cmd);
}

void HookDrawablePresentAtTime(id self, SEL cmd, CFTimeInterval time) {
  const uint64_t n = ++g_drawable_present_at_time_calls;
  if (n <= 5) {
    std::fprintf(stderr, "[probe] drawable presentAtTime #%llu t=%.6f now=%.6f\n",
                 (unsigned long long)n, time, CACurrentMediaTime());
  }
  if (g_force_drawable_present && g_orig_drawable_present) {
    reinterpret_cast<void (*)(id, SEL)>(g_orig_drawable_present)(
        self, sel_registerName("present"));
    return;
  }
  reinterpret_cast<void (*)(id, SEL, CFTimeInterval)>(g_orig_drawable_present_at_time)(
      self, cmd, time);
}

void InstallDrawableHooks(id drawable) {
  if (g_drawable_hooks_installed.exchange(true)) return;
  Class c = object_getClass(drawable);
  g_orig_drawable_present = Swizzle(c, "present", (IMP)HookDrawablePresent);
  g_orig_drawable_present_at_time =
      Swizzle(c, "presentAtTime:", (IMP)HookDrawablePresentAtTime);
  std::fprintf(stderr,
               "[probe] drawable present hooks on %s: present=%d presentAtTime=%d force=%d\n",
               class_getName(c), g_orig_drawable_present != nullptr,
               g_orig_drawable_present_at_time != nullptr,
               g_force_drawable_present ? 1 : 0);
}

bool InstallLayerHooks() {
  if (g_layer_hooks_installed.exchange(true)) return true;
  Class layer_class = objc_getClass("CAMetalLayer");
  if (!layer_class) {
    // Metal/QuartzCore may not be registered yet when the static constructor runs.
    g_layer_hooks_installed = false;
    return false;
  }
  g_orig_set_drawable_size = Swizzle(layer_class, "setDrawableSize:", (IMP)HookSetDrawableSize);
  g_orig_next_drawable = Swizzle(layer_class, "nextDrawable", (IMP)HookNextDrawable);
  g_orig_set_fb_only = Swizzle(layer_class, "setFramebufferOnly:", (IMP)HookSetFramebufferOnly);
  return g_orig_set_drawable_size != nullptr;
}

__attribute__((constructor)) void ProbeEarlyInit() {
  g_enabled = EnvInt("MGSV_PROBE", 0) != 0;
  if (!g_enabled) return;
  if (const char* p = std::getenv("MGSV_DRAWABLE_POLICY")) {
    if (!std::strcmp(p, "off")) g_policy = DrawablePolicy::kOff;
    else if (!std::strcmp(p, "substitute")) g_policy = DrawablePolicy::kSubstitute;
  }
  g_log_every = EnvInt("MGSV_PROBE_LOG_EVERY", 120);
  g_readback_every = EnvInt("MGSV_READBACK", 0);
  g_force_drawable_present = EnvInt("MGSV_FORCE_DRAWABLE_PRESENT", 0) != 0;
  const bool hooked = InstallLayerHooks();
  std::fprintf(stderr, "[probe] layer hooks %s (policy=%d readback_every=%d)\n",
               hooked ? "installed" : "deferred (CAMetalLayer not loaded yet)",
               (int)g_policy, g_readback_every);
}

NSWindow* FindNSWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (!windows || count == 0) return nil;
  void* p = SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]),
                                   SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
  SDL_free(windows);
  return (__bridge NSWindow*)p;
}

CAMetalLayer* FindMetalLayer(NSView* v) {
  if ([v.layer isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer*)v.layer;
  for (NSView* s in v.subviews) {
    if (CAMetalLayer* r = FindMetalLayer(s)) return r;
  }
  return nil;
}

// Compositor ladder. Runs on the UI thread.
void AddCompositorTests(NSWindow* win) {
  NSView* cv = win.contentView;
  const CGFloat scale = win.backingScaleFactor;
  const NSRect b = cv.bounds;

  // (a) Plain CALayer-backed view, no Metal at all.
  NSView* magenta = [[NSView alloc] initWithFrame:NSMakeRect(10, 10, 160, 160)];
  magenta.wantsLayer = YES;
  magenta.layer.backgroundColor = [NSColor magentaColor].CGColor;
  [cv addSubview:magenta positioned:NSWindowAbove relativeTo:nil];

  // (b) A second CAMetalLayer driven by raw Metal, same window, same process.
  NSView* mv = [[NSView alloc] initWithFrame:NSMakeRect(b.size.width - 170, 10, 160, 160)];
  CAMetalLayer* ml = [CAMetalLayer layer];
  ml.device = MTLCreateSystemDefaultDevice();
  ml.pixelFormat = MTLPixelFormatBGRA8Unorm;
  ml.contentsScale = scale;
  ml.drawableSize = CGSizeMake(160 * scale, 160 * scale);
  ml.opaque = YES;
  mv.layer = ml;
  mv.wantsLayer = YES;
  [cv addSubview:mv positioned:NSWindowAbove relativeTo:nil];
  g_test_layer = ml;

  id<MTLCommandQueue> queue = [ml.device newCommandQueue];
  g_test_thread = std::thread([ml, queue] {
    int tick = 0;
    while (!g_stop) {
      @autoreleasepool {
        id<CAMetalDrawable> d = [ml nextDrawable];
        if (d) {
          MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
          rp.colorAttachments[0].texture = d.texture;
          rp.colorAttachments[0].loadAction = MTLLoadActionClear;
          rp.colorAttachments[0].storeAction = MTLStoreActionStore;
          rp.colorAttachments[0].clearColor =
              (tick++ & 1) ? MTLClearColorMake(0, 1, 1, 1) : MTLClearColorMake(1, 1, 0, 1);
          id<MTLCommandBuffer> cb = [queue commandBuffer];
          [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
          [cb presentDrawable:d];
          [cb commit];
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  });
  std::fprintf(stderr, "[probe] compositor test views added: magenta (bottom-left), "
                       "raw-Metal cyan/yellow flasher (bottom-right)\n");
}

// --- capture view (host bring-up) -------------------------------------------
//
// A plain CALayer-backed NSView whose `contents` is a brightened copy of the
// guest output. The compositor test above proved this window composites such
// views, so this is a dependable way to see the game while the presenter's
// UI-drawer pass is being diagnosed.

static NSView* g_capture_view = nil;

extern "C" void MgsvCaptureViewCreate(void) {
  if (g_capture_view) return;
  NSWindow* win = FindNSWindow();
  if (!win) {
    std::fprintf(stderr, "[capture-view] no NSWindow\n");
    return;
  }
  NSView* cv = win.contentView;
  NSView* view = [[NSView alloc] initWithFrame:cv.bounds];
  view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  view.wantsLayer = YES;
  CALayer* layer = view.layer;
  layer.backgroundColor = [NSColor blackColor].CGColor;
  layer.contentsGravity = kCAGravityResizeAspect;
  layer.magnificationFilter = kCAFilterLinear;
  layer.minificationFilter = kCAFilterLinear;
  [cv addSubview:view positioned:NSWindowAbove relativeTo:nil];
  g_capture_view = view;
  std::fprintf(stderr, "[capture-view] added %.0fx%.0f view over contentView\n",
               cv.bounds.size.width, cv.bounds.size.height);
}

extern "C" void MgsvCaptureViewUpdate(const void* rgba, int width, int height, int stride) {
  NSView* view = g_capture_view;
  if (!view || !rgba || width <= 0 || height <= 0 || stride < width * 4) return;
  CFDataRef data =
      CFDataCreate(kCFAllocatorDefault, (const UInt8*)rgba, (CFIndex)stride * height);
  if (!data) return;
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CFRelease(data);
  if (!provider) return;
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  // The capture path writes R,G,B,A; ignore alpha.
  CGBitmapInfo bitmap_info =
      (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault;
  CGImageRef image = CGImageCreate((size_t)width, (size_t)height, 8, 32, (size_t)stride, cs,
                                   bitmap_info, provider, nullptr, false,
                                   kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(provider);
  if (!image) return;
  id contents = CFBridgingRelease(image);  // ARC-managed from here; the layer retains it too.
  dispatch_async(dispatch_get_main_queue(), ^{
    view.layer.contents = contents;
  });
}

void InstallMainThread(void*) {
  NSWindow* win = FindNSWindow();
  if (!win) {
    std::fprintf(stderr, "[probe] no NSWindow\n");
    return;
  }
  // Make the layer MoltenVK already configured sampleable, and log its state once.
  if (CAMetalLayer* l = FindMetalLayer(win.contentView)) {
    if (g_readback_every > 0) l.framebufferOnly = NO;
    std::fprintf(stderr,
                 "[probe] existing layer %p: drawableSize=%.0fx%.0f pixelFormat=%lu colorspace=%s "
                 "edr=%d opaque=%d fbOnly=%d maxDrawables=%lu\n",
                 (__bridge void*)l, l.drawableSize.width, l.drawableSize.height, (unsigned long)l.pixelFormat,
                 ColorspaceName(l.colorspace).c_str(), l.wantsExtendedDynamicRangeContent ? 1 : 0,
                 l.isOpaque ? 1 : 0, l.framebufferOnly ? 1 : 0,
                 (unsigned long)l.maximumDrawableCount);
  }
  if (EnvInt("MGSV_COMPOSITOR_TEST", 0)) AddCompositorTests(win);
}

}  // namespace

extern "C" void MgsvPresentProbeInstall(void) {
  if (!g_enabled || g_installed.exchange(true)) return;
  // Retry here in case the static constructor ran before Metal was registered.
  InstallLayerHooks();
  // Hook presentDrawable: on whatever concrete command-buffer class this device vends.
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  id<MTLCommandQueue> q = [dev newCommandQueue];
  id<MTLCommandBuffer> cb = [q commandBuffer];
  g_orig_present_drawable =
      Swizzle(object_getClass(cb), "presentDrawable:", (IMP)HookPresentDrawable);
  g_orig_present_at_time = Swizzle(object_getClass(cb), "presentDrawable:atTime:",
                                   (IMP)HookPresentDrawableAtTime);
  g_orig_present_after_duration = Swizzle(
      object_getClass(cb), "presentDrawable:afterMinimumDuration:",
      (IMP)HookPresentDrawableAfterMinimumDuration);
  std::fprintf(stderr,
               "[probe] present hooks on %s: present=%d atTime=%d afterMinDuration=%d\n",
               object_getClassName(cb), g_orig_present_drawable != nullptr,
               g_orig_present_at_time != nullptr, g_orig_present_after_duration != nullptr);
  SDL_RunOnMainThread(InstallMainThread, nullptr, true);
}

extern "C" void MgsvPresentProbeShutdown(void) {
  g_stop = true;
  if (g_test_thread.joinable()) g_test_thread.join();
  if (g_enabled) {
    std::fprintf(stderr,
                 "[probe] totals: setDrawableSize=%llu (degenerate=%llu) nextDrawable=%llu "
                 "(nil=%llu) present=%llu atTime=%llu afterMinDuration=%llu "
                 "drawablePresent=%llu drawablePresentAtTime=%llu\n",
                 (unsigned long long)g_set_size_calls.load(),
                 (unsigned long long)g_degenerate_calls.load(),
                 (unsigned long long)g_next_calls.load(), (unsigned long long)g_next_nil.load(),
                 (unsigned long long)g_present_calls.load(),
                 (unsigned long long)g_present_at_time_calls.load(),
                 (unsigned long long)g_present_after_calls.load(),
                 (unsigned long long)g_drawable_present_calls.load(),
                 (unsigned long long)g_drawable_present_at_time_calls.load());
  }
}
