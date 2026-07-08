// Chroma Shift — directional RGB channel separation with per-band jitter.
// R and B are sampled at opposite offsets along an angle (classic chromatic
// aberration), and horizontal bands each get their own hashed extra split, so
// the fringe judders in strips instead of sliding uniformly.
//
// Pure pixel moves (lossless at any bit depth); all randomness is a hash of
// (seed, frame/hold, band). Runs on the host's render thread. OFX rows are
// bottom-up; bands are counted in top-down space.

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

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}
static inline uint32_t hash2(uint32_t a, uint32_t b) { return hash32(a * 0x9E3779B9u ^ hash32(b)); }
static inline float hashf(uint32_t h) { return (h >> 8) * (1.0f / 16777216.0f); } // [0,1)
} // namespace

// ---------------------------------------------------------------------------
class ChromaShiftProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit ChromaShiftProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  inline void *srcAt(int lx, int ly) const {
    lx = clampv(lx, 0, _W - 1); ly = clampv(ly, 0, _H - 1);
    return _src->getPixelAddress(_bounds.x1 + lx, _bounds.y2 - 1 - ly);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float amount, float angleDeg, int bandH,
               float jitter, uint32_t state) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    const float a = angleDeg * 3.14159265f / 180.0f;
    const int ox = (int)std::lround(std::cos(a) * amount);
    const int oy = (int)std::lround(std::sin(a) * amount); // top-down y

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down row
      const int band = clampv(ly, 0, _H - 1) / bandH;
      const int jx = (int)std::lround((hashf(hash2(state, (uint32_t)band)) * 2.0f - 1.0f) * jitter);

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        PIX *pc = (PIX *)srcAt(lx, ly);
        PIX *pr = (PIX *)srcAt(lx - ox - jx, ly - oy);
        PIX *pb = (PIX *)srcAt(lx + ox + jx, ly + oy);
        if (!pc) { for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
        dst[0] = pr ? pr[0] : pc[0];
        dst[1] = pc[1];
        dst[2] = pb ? pb[2] : pc[2];
        if (nComps == 4) dst[3] = pc[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class ChromaShiftPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_angle = nullptr, *_jitter = nullptr;
  OFX::IntParam *_band = nullptr, *_hold = nullptr, *_seed = nullptr;
public:
  explicit ChromaShiftPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount");
    _angle = fetchDoubleParam("angle");
    _band = fetchIntParam("band");
    _jitter = fetchDoubleParam("jitter");
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

    double amount, angle, jitter; int band, hold, seed;
    _amount->getValueAtTime(args.time, amount);
    _angle->getValueAtTime(args.time, angle);
    _band->getValueAtTime(args.time, band);
    _jitter->getValueAtTime(args.time, jitter);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);

    // pixel-unit params scale for proxy/half-res renders
    const double rs = args.renderScale.x;
    amount *= rs; jitter *= rs;
    band = std::max(1, (int)std::lround(band * rs));

    const uint32_t frame = (uint32_t)std::lround(std::max(0.0, args.time));
    const uint32_t state = hash2((uint32_t)seed * 0x9E3779B9u + 2u, frame / (uint32_t)std::max(1, hold));

    ChromaShiftProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)amount,   \
                     (float)angle, band, (float)jitter, state);                   \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)amount,   \
                     (float)angle, band, (float)jitter, state);                   \
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
mDeclarePluginFactory(ChromaShiftFactory, {}, {});

using namespace OFX;

void ChromaShiftFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Chroma Shift", "Chroma Shift", "Chroma Shift");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // channels sample outside the window
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void ChromaShiftFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount");
  am->setRange(0, 64); am->setDisplayRange(0, 32); am->setDefault(6);

  DoubleParamDescriptor *an = desc.defineDoubleParam("angle");
  an->setLabels("Angle", "Angle", "Angle");
  an->setRange(0, 360); an->setDisplayRange(0, 360); an->setDefault(0);

  IntParamDescriptor *bd = desc.defineIntParam("band");
  bd->setLabels("Band height", "Band height", "Band height");
  bd->setRange(1, 256); bd->setDisplayRange(4, 128); bd->setDefault(32);

  DoubleParamDescriptor *ji = desc.defineDoubleParam("jitter");
  ji->setLabels("Band jitter", "Band jitter", "Band jitter");
  ji->setRange(0, 128); ji->setDisplayRange(0, 64); ji->setDefault(12);

  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold frames", "Hold frames", "Hold frames");
  ho->setRange(1, 10); ho->setDisplayRange(1, 10); ho->setDefault(2);

  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed");
  se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(0);
}

OFX::ImageEffect *ChromaShiftFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ChromaShiftPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ChromaShiftFactory p("org.purzos.chromaShift", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
