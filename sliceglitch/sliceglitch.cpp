// Slice Glitch — horizontal slice displacement, the classic "torn scanlines"
// glitch. Rows are grouped into slices; a hash decides per slice whether it
// tears, how far it shifts (wrapping around the frame), and how much the R/B
// channels split inside the tear.
//
// All randomness is a pure hash of (seed, frame/hold, slice) — no rand(), so
// renders are deterministic and identical on any machine. Everything runs on
// the host's render thread. OFX rows are bottom-up; slices are counted in
// top-down space (`ly`) so the effect reads naturally and matches any future
// web port.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int wrapi(int v, int n) { v %= n; return v < 0 ? v + n : v; }

// lowbias32 — a well-mixed 32-bit integer hash (deterministic across builds).
static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static inline uint32_t hash2(uint32_t a, uint32_t b) { return hash32(a * 0x9E3779B9u ^ hash32(b)); }
static inline float hashf(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); } // [0,1)
} // namespace

// ---------------------------------------------------------------------------
class SliceGlitchProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit SliceGlitchProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  // logical top-down coords, clamped to the frame
  inline void *srcAt(int lx, int ly) const {
    lx = clampv(lx, 0, _W - 1); ly = clampv(ly, 0, _H - 1);
    return _src->getPixelAddress(_bounds.x1 + lx, _bounds.y2 - 1 - ly);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, int sliceH, float density, float maxShift,
               float split, uint32_t state) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down row
      const int slice = clampv(ly, 0, _H - 1) / sliceH;

      const uint32_t h = hash2(state, (uint32_t)slice);
      int dxs = 0, sp = 0;
      if (hashf(h) < density) {
        dxs = (int)std::lround((hashf(hash32(h ^ 0x9E3779B9u)) * 2.0f - 1.0f) * maxShift);
        sp  = (int)std::lround(hashf(hash32(h ^ 0x85EBCA6Bu)) * split);
      }

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        const int cx = wrapi(lx - dxs, _W);
        PIX *pc = (PIX *)srcAt(cx, ly);
        if (!pc) { for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
        if (sp) { // channel split inside the tear (pure pixel moves — lossless)
          PIX *pr = (PIX *)srcAt(wrapi(cx - sp, _W), ly);
          PIX *pb = (PIX *)srcAt(wrapi(cx + sp, _W), ly);
          dst[0] = pr ? pr[0] : pc[0];
          dst[1] = pc[1];
          dst[2] = pb ? pb[2] : pc[2];
        } else {
          dst[0] = pc[0]; dst[1] = pc[1]; dst[2] = pc[2];
        }
        if (nComps == 4) dst[3] = pc[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class SliceGlitchPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_slice = nullptr, *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_density = nullptr, *_shift = nullptr, *_split = nullptr;
public:
  explicit SliceGlitchPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _slice = fetchIntParam("slice");
    _density = fetchDoubleParam("density");
    _shift = fetchDoubleParam("shift");
    _split = fetchDoubleParam("split");
    _hold = fetchIntParam("hold");
    _seed = fetchIntParam("seed");
  }

  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4
                 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int slice, hold, seed; double density, shift, split;
    _slice->getValueAtTime(args.time, slice);
    _density->getValueAtTime(args.time, density);
    _shift->getValueAtTime(args.time, shift);
    _split->getValueAtTime(args.time, split);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);

    // pixel-unit params scale for proxy/half-res renders
    const double rs = args.renderScale.x;
    slice = std::max(1, (int)std::lround(slice * rs));
    shift *= rs; split *= rs;

    const uint32_t frame = (uint32_t)std::lround(std::max(0.0, args.time));
    const uint32_t state = hash2((uint32_t)seed * 0x9E3779B9u + 1u, frame / (uint32_t)std::max(1, hold));

    SliceGlitchProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, slice,           \
                     (float)density, (float)shift, (float)split, state);          \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, slice,           \
                     (float)density, (float)shift, (float)split, state);          \
    } while (0)

    switch (depth) {
      case OFX::eBitDepthUByte:  DISPATCH(unsigned char, 255); break;
      case OFX::eBitDepthUShort: DISPATCH(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  DISPATCH(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef DISPATCH
  }
};

// ---------------------------------------------------------------------------
mDeclarePluginFactory(SliceGlitchFactory, {}, {});

using namespace OFX;

void SliceGlitchFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Slice Glitch", "Slice Glitch", "Slice Glitch");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // shifted slices wrap the full width
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void SliceGlitchFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  IntParamDescriptor *sl = desc.defineIntParam("slice");
  sl->setLabels("Slice height", "Slice height", "Slice height");
  sl->setRange(2, 256); sl->setDisplayRange(4, 96); sl->setDefault(24);

  DoubleParamDescriptor *de = desc.defineDoubleParam("density");
  de->setLabels("Density", "Density", "Density");
  de->setRange(0, 1); de->setDisplayRange(0, 1); de->setDefault(0.35);

  DoubleParamDescriptor *sh = desc.defineDoubleParam("shift");
  sh->setLabels("Max shift", "Max shift", "Max shift");
  sh->setRange(0, 512); sh->setDisplayRange(0, 256); sh->setDefault(96);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("split");
  sp->setLabels("Chroma split", "Chroma split", "Chroma split");
  sp->setRange(0, 64); sp->setDisplayRange(0, 32); sp->setDefault(12);

  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold frames", "Hold frames", "Hold frames");
  ho->setRange(1, 10); ho->setDisplayRange(1, 10); ho->setDefault(2);

  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed");
  se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(0);
}

OFX::ImageEffect *SliceGlitchFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new SliceGlitchPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static SliceGlitchFactory p("org.purzos.sliceGlitch", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
