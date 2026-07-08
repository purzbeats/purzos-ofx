// Sync Drift — a TV losing its grip on the signal. Three sync failures, all
// whole-row moves:
//   • SHEAR    — horizontal hold slipping: each row lands a little further
//     over than the last, wrapping — the iconic diagonal lean, and at higher
//     values the full "picture torn into diagonal bars",
//   • FLAG     — flagging/skew at the top of the frame (timebase error every
//     VCR-to-TV hookup had), gently waving,
//   • ROLL     — vertical hold slipping: the whole picture rolls, wrapping
//     top-to-bottom, with the dark blanking bar sweeping through.
//
// Deterministic — pure functions of (row, frame), no randomness at all. Runs
// on the host's render thread. OFX rows are bottom-up; shear/flag/roll are
// computed in top-down space so the picture leans and rolls the way a real
// raster does.

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
} // namespace

// ---------------------------------------------------------------------------
class SyncDriftProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit SyncDriftProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float shear, float flag, int flagH,
               float rollFrac, float t) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    const int rollOff = (int)std::lround(rollFrac * _H);
    const int barH = std::max(2, _H / 24); // blanking bar thickness

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down row

      // v-roll: which source row this scanline shows (wraps)
      const int sy = wrapi(ly + rollOff, _H);
      // the blanking bar rides the wrap seam
      const bool inBar = rollOff != 0 && sy >= _H - barH;

      // h-shear accumulates down the frame; flagging bends the top rows
      float dx = shear * (float)ly;
      if (ly < flagH) {
        const float k = 1.0f - (float)ly / (float)flagH;
        dx += flag * k * k * (0.75f + 0.25f * std::sin(t * 1.7f));
      }
      const int idx = (int)std::lround(dx);

      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, _bounds.y2 - 1 - sy);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        PIX *p = row + wrapi(lx - idx, _W) * nComps;
        if (inBar) { // blanking: the real pixels, crushed almost to black
          dst[0] = (PIX)(p[0] / 6);
          dst[1] = (PIX)(p[1] / 6);
          dst[2] = (PIX)(p[2] / 6);
        } else {
          dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2];
        }
        if (nComps == 4) dst[3] = p[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class SyncDriftPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_shear = nullptr, *_flag = nullptr, *_roll = nullptr, *_speed = nullptr;
  OFX::IntParam *_flagH = nullptr;
public:
  explicit SyncDriftPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _shear = fetchDoubleParam("shear");
    _flag = fetchDoubleParam("flag");
    _flagH = fetchIntParam("flagH");
    _roll = fetchDoubleParam("roll");
    _speed = fetchDoubleParam("speed");
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

    double shear, flag, roll, speed; int flagH;
    _shear->getValueAtTime(args.time, shear);
    _flag->getValueAtTime(args.time, flag);
    _flagH->getValueAtTime(args.time, flagH);
    _roll->getValueAtTime(args.time, roll);
    _speed->getValueAtTime(args.time, speed);

    // shear is px-per-row: proxy scales px AND row count, so it's scale-free.
    // flag offset + height are pixel units.
    const double rs = args.renderScale.x;
    flag *= rs;
    flagH = std::max(1, (int)std::lround(flagH * rs));

    const float t = (float)(args.time * speed);
    // rolls/second at a nominal 30fps timeline -> fraction of a frame height
    const float rollFrac = roll > 0.0
      ? (float)std::fmod(args.time * speed * roll / 30.0, 1.0) : 0.0f;

    SyncDriftProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)shear,    \
                     (float)flag, flagH, rollFrac, t);                            \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)shear,    \
                     (float)flag, flagH, rollFrac, t);                            \
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
mDeclarePluginFactory(SyncDriftFactory, {}, {});

using namespace OFX;

void SyncDriftFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Sync Drift", "Sync Drift", "Sync Drift");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // rows wrap both axes
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void SyncDriftFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *sh = desc.defineDoubleParam("shear");
  sh->setLabels("Shear", "Shear", "Shear");
  sh->setRange(-3, 3); sh->setDisplayRange(-1, 1); sh->setDefault(0.15);

  DoubleParamDescriptor *fg = desc.defineDoubleParam("flag");
  fg->setLabels("Flagging", "Flagging", "Flagging");
  fg->setRange(0, 64); fg->setDisplayRange(0, 32); fg->setDefault(12);

  IntParamDescriptor *fh = desc.defineIntParam("flagH");
  fh->setLabels("Flag height", "Flag height", "Flag height");
  fh->setRange(4, 256); fh->setDisplayRange(16, 128); fh->setDefault(48);

  DoubleParamDescriptor *ro = desc.defineDoubleParam("roll");
  ro->setLabels("Roll", "Roll", "Roll");
  ro->setRange(0, 4); ro->setDisplayRange(0, 2); ro->setDefault(0);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed");
  sp->setRange(0, 4); sp->setDisplayRange(0, 2); sp->setDefault(1);
}

OFX::ImageEffect *SyncDriftFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new SyncDriftPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static SyncDriftFactory p("org.purzos.syncDrift", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
