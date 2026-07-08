// Video Feedback — a camera pointed at its own monitor. Every output pixel
// accumulates a stack of nested copies of the frame, each one zoomed in,
// rotated a little further, and dimmer than the last — the infinite corridor
// of every 80s music video and public-access ident. SPIN rotates the whole
// stack over time (deeper copies turn faster, exactly like real feedback).
//
// Pure resampling of the frame against itself — no randomness, fully
// deterministic. Nearest-neighbour taps, per house style. Runs on the host's
// render thread. Rotation happens about the centre, so OFX's bottom-up rows
// only flip the direction of spin — we convert to top-down so positive spin
// turns clockwise.

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
} // namespace

// ---------------------------------------------------------------------------
class FeedbackProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit FeedbackProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, float zoom, float anglePerTap, int taps,
               float decay, float cxf, float cyf) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;
    const float cx = cxf * (_W - 1), cy = cyf * (_H - 1);
    const float ca = std::cos(-anglePerTap), sa = std::sin(-anglePerTap);

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y; // top-down

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        float accR = 0, accG = 0, accB = 0, accA = 0, wsum = 0;

        // tap 0 is the frame itself; each deeper tap un-zooms and un-rotates
        float px = lx - cx, py = ly - cy;
        float w = 1.0f;
        for (int k = 0; k < taps; k++) {
          const int sx = (int)std::lround(cx + px);
          const int sy = (int)std::lround(cy + py);
          if (sx >= 0 && sx < _W && sy >= 0 && sy < _H) {
            PIX *p = (PIX *)_src->getPixelAddress(_bounds.x1 + sx, _bounds.y2 - 1 - sy);
            if (p) {
              accR += w * p[0]; accG += w * p[1]; accB += w * p[2];
              if (nComps == 4) accA += w * p[3];
              wsum += w;
            }
          }
          // next copy: bigger source coords (deeper = zoomed in), turned back
          const float nx = (px * ca - py * sa) / zoom;
          const float ny = (px * sa + py * ca) / zoom;
          px = nx; py = ny;
          w *= decay;
        }

        if (wsum > 0.0f) {
          const float inv = 1.0f / (wsum * (float)maxv);
          dst[0] = q<PIX, nComps, maxv>(accR * inv);
          dst[1] = q<PIX, nComps, maxv>(accG * inv);
          dst[2] = q<PIX, nComps, maxv>(accB * inv);
          if (nComps == 4) dst[3] = q<PIX, nComps, maxv>(accA * inv);
        } else {
          for (int c = 0; c < nComps; c++) dst[c] = 0;
        }
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class FeedbackPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_zoom = nullptr, *_rotate = nullptr, *_decay = nullptr,
                   *_spin = nullptr, *_cx = nullptr, *_cy = nullptr;
  OFX::IntParam *_taps = nullptr;
public:
  explicit FeedbackPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _zoom = fetchDoubleParam("zoom");
    _rotate = fetchDoubleParam("rotate");
    _taps = fetchIntParam("taps");
    _decay = fetchDoubleParam("decay");
    _spin = fetchDoubleParam("spin");
    _cx = fetchDoubleParam("cx");
    _cy = fetchDoubleParam("cy");
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

    double zoom, rotate, decay, spin, cx, cy; int taps;
    _zoom->getValueAtTime(args.time, zoom);
    _rotate->getValueAtTime(args.time, rotate);
    _taps->getValueAtTime(args.time, taps);
    _decay->getValueAtTime(args.time, decay);
    _spin->getValueAtTime(args.time, spin);
    _cx->getValueAtTime(args.time, cx);
    _cy->getValueAtTime(args.time, cy);

    // spin adds degrees-per-frame to the per-tap angle: the stack corkscrews
    const float angle = (float)((rotate + spin * args.time) * 3.14159265 / 180.0);

    FeedbackProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, (float)zoom,     \
                     angle, taps, (float)decay, (float)cx, (float)cy);            \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, (float)zoom,     \
                     angle, taps, (float)decay, (float)cx, (float)cy);            \
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
mDeclarePluginFactory(FeedbackFactory, {}, {});

using namespace OFX;

void FeedbackFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Video Feedback", "Video Feedback", "Video Feedback");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // taps sample the whole frame
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void FeedbackFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  DoubleParamDescriptor *zo = desc.defineDoubleParam("zoom");
  zo->setLabels("Zoom", "Zoom", "Zoom");
  zo->setRange(0.5, 0.98); zo->setDisplayRange(0.6, 0.95); zo->setDefault(0.82);

  DoubleParamDescriptor *ro = desc.defineDoubleParam("rotate");
  ro->setLabels("Rotate/tap", "Rotate/tap", "Rotate/tap");
  ro->setRange(-45, 45); ro->setDisplayRange(-20, 20); ro->setDefault(6);

  IntParamDescriptor *ta = desc.defineIntParam("taps");
  ta->setLabels("Depth", "Depth", "Depth");
  ta->setRange(2, 24); ta->setDisplayRange(2, 16); ta->setDefault(8);

  DoubleParamDescriptor *de = desc.defineDoubleParam("decay");
  de->setLabels("Decay", "Decay", "Decay");
  de->setRange(0.3, 1); de->setDisplayRange(0.5, 1); de->setDefault(0.8);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("spin");
  sp->setLabels("Spin", "Spin", "Spin");
  sp->setRange(-10, 10); sp->setDisplayRange(-3, 3); sp->setDefault(0.4);

  DoubleParamDescriptor *px = desc.defineDoubleParam("cx");
  px->setLabels("Centre X", "Centre X", "Centre X");
  px->setRange(0, 1); px->setDisplayRange(0, 1); px->setDefault(0.5);

  DoubleParamDescriptor *py = desc.defineDoubleParam("cy");
  py->setLabels("Centre Y", "Centre Y", "Centre Y");
  py->setRange(0, 1); py->setDisplayRange(0, 1); py->setDefault(0.5);
}

OFX::ImageEffect *FeedbackFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new FeedbackPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static FeedbackFactory p("org.purzos.videoFeedback", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
