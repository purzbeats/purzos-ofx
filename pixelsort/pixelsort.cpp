// Pixel Sort — OFX port of purzos/tools/effects/pixelsort/effect.js
//
// The classic glitch: sort contiguous runs of pixels by brightness along each
// row (or column), but only on one side of a brightness threshold, so flat
// regions smear into clean gradients while the rest stays put.
//
// It reorders whole pixels (RGBA together). The permutation is built in one
// pass (in the web tool's top-down orientation), then the write copies
// source->dest through it; both run on the host's render thread. Pixel-type
// agnostic: the permutation is pure index math.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <vector>
#include <algorithm>
#include <cmath>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

// ---------------------------------------------------------------------------
class PixelSortProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
  std::vector<int> _perm;   // logical (top-down) output idx -> source logical idx
public:
  explicit PixelSortProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  // OFX y is bottom-up; we work in a logical top-down image (ly=0 is the top)
  // so the result matches the web effect exactly. ofxY converts back.
  inline int ofxY(int ly) const { return _bounds.y2 - 1 - ly; }

  template <class PIX, int nComps, int maxv>
  void buildPerm(int direction, float threshold, bool bright, bool desc) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    std::vector<float> L((size_t)_W * _H);
    for (int ly = 0; ly < _H; ly++) {
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, ofxY(ly));
      for (int lx = 0; lx < _W; lx++) {
        PIX *p = row ? row + lx * nComps : nullptr;
        L[(size_t)ly * _W + lx] = p
          ? (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv : 0.0f;
      }
    }

    _perm.resize((size_t)_W * _H);
    for (size_t i = 0; i < _perm.size(); i++) _perm[i] = (int)i;

    auto inRun = [&](float l) { return bright ? (l >= threshold) : (l <= threshold); };
    std::vector<int> run;
    auto sortRun = [&](int base, int len, int step) {
      run.resize(len);
      for (int k = 0; k < len; k++) run[k] = base + k * step;
      // stable: JS Array.prototype.sort keeps spatial order for equal luma —
      // std::sort would shuffle equal-luma pixels differently per build/frame
      std::stable_sort(run.begin(), run.end(), [&](int a, int b) {
        return desc ? (L[a] > L[b]) : (L[a] < L[b]);
      });
      for (int k = 0; k < len; k++) _perm[base + k * step] = run[k];
    };

    if (direction == 0) { // horizontal: sort runs along each row
      for (int ly = 0; ly < _H; ly++) {
        const int rowo = ly * _W;
        int x = 0;
        while (x < _W) {
          if (inRun(L[rowo + x])) {
            int s = x;
            while (x < _W && inRun(L[rowo + x])) x++;
            sortRun(rowo + s, x - s, 1);
          } else x++;
        }
      }
    } else {              // vertical: sort runs down each column (stride W)
      for (int lx = 0; lx < _W; lx++) {
        int y = 0;
        while (y < _H) {
          if (inRun(L[(size_t)y * _W + lx])) {
            int s = y;
            while (y < _H && inRun(L[(size_t)y * _W + lx])) y++;
            sortRun(s * _W + lx, y - s, _W);
          } else y++;
        }
      }
    }
  }

  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win) {
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = _bounds.y2 - 1 - y;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = x - _bounds.x1;
        if (ly < 0 || ly >= _H || lx < 0 || lx >= _W) { for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }
        const int s = _perm[(size_t)ly * _W + lx];
        PIX *sp = (PIX *)_src->getPixelAddress(_bounds.x1 + (s % _W), ofxY(s / _W));
        if (sp) for (int c = 0; c < nComps; c++) dst[c] = sp[c];
        else    for (int c = 0; c < nComps; c++) dst[c] = 0;
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class PixelSortPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_direction = nullptr, *_mode = nullptr, *_order = nullptr;
  OFX::DoubleParam *_threshold = nullptr;
public:
  explicit PixelSortPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _direction = fetchChoiceParam("direction");
    _threshold = fetchDoubleParam("threshold");
    _mode = fetchChoiceParam("mode");
    _order = fetchChoiceParam("order");
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

    int direction, mode, order; double threshold;
    _direction->getValueAtTime(args.time, direction);
    _threshold->getValueAtTime(args.time, threshold);
    _mode->getValueAtTime(args.time, mode);
    _order->getValueAtTime(args.time, order);
    const bool bright = (mode == 0);
    const bool desc = (order == 1);

    PixelSortProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                         \
    do {                                                                           \
      if (nc == 4) { proc.buildPerm<PIX, 4, MAXV>(direction, (float)threshold, bright, desc); \
                     proc.writePixels<PIX, 4, MAXV>(args.renderWindow); }          \
      else         { proc.buildPerm<PIX, 3, MAXV>(direction, (float)threshold, bright, desc); \
                     proc.writePixels<PIX, 3, MAXV>(args.renderWindow); }          \
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
mDeclarePluginFactory(PixelSortFactory, {}, {});

using namespace OFX;

void PixelSortFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Pixel Sort", "Pixel Sort", "Pixel Sort");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // a sorted run spans the whole row/column
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void PixelSortFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  ChoiceParamDescriptor *dir = desc.defineChoiceParam("direction");
  dir->setLabels("Direction", "Direction", "Direction");
  dir->appendOption("Horizontal");
  dir->appendOption("Vertical");
  dir->setDefault(0);

  DoubleParamDescriptor *thr = desc.defineDoubleParam("threshold");
  thr->setLabels("Threshold", "Threshold", "Threshold");
  thr->setRange(0, 1); thr->setDisplayRange(0, 1); thr->setDefault(0.5);

  ChoiceParamDescriptor *mode = desc.defineChoiceParam("mode");
  mode->setLabels("Sort where", "Sort where", "Sort where");
  mode->appendOption("Brighter than");
  mode->appendOption("Darker than");
  mode->setDefault(0);

  ChoiceParamDescriptor *order = desc.defineChoiceParam("order");
  order->setLabels("Order", "Order", "Order");
  order->appendOption("Dark to bright");
  order->appendOption("Bright to dark");
  order->setDefault(0);
}

OFX::ImageEffect *PixelSortFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new PixelSortPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static PixelSortFactory p("org.purzos.pixelSort", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
