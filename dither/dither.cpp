// Bayer Dither — OFX port of purzos/tools/effects/dither/effect.js
//
// Ordered 4x4 dithering down to a small retro palette with chunky pixels.
// The web effect is a pure function of (source, params); this keeps that:
// build a low-res cell grid of palette indices from the whole frame, then map
// every output pixel back through it (nearest-neighbour, crisp squares).
//
// Both passes run on the host's render thread (we don't use the Support
// library's ImageProcessor threading); the grid is immutable once built.
//
// OFX rows are bottom-up but the web effect works top-down, so pixel<->cell
// maps convert through `ty` — the partition and the Bayer row index are only
// web-identical in top-down space.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <vector>
#include <cmath>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {

// --- palettes (dark->light ramps), straight from effect.js, sRGB 0..255 -----
struct Palette { const char *id; int n; float rgb[4][3]; };
static const Palette PALETTES[] = {
  {"gameboy", 4, {{15,56,15},{48,98,48},{139,172,15},{155,188,15}}},
  {"mono",    2, {{15,15,25},{235,235,245}}},
  {"amber",   4, {{20,10,0},{120,60,0},{200,120,10},{255,190,80}}},
  {"ice",     4, {{10,16,30},{40,80,130},{90,150,200},{200,235,255}}},
  {"ember",   4, {{10,4,8},{120,20,30},{220,80,30},{255,200,90}}},
};
static const int N_PALETTES = sizeof(PALETTES) / sizeof(PALETTES[0]);

// 4x4 Bayer matrix, normalised to ~[-0.5, 0.5]
static float bayer(int i) {
  static const int B[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
  return (B[i] + 0.5f) / 16.0f - 0.5f;
}

template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

// ---------------------------------------------------------------------------
// Processor: holds the precomputed cell grid + chosen palette; writes pixels.
class DitherProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};                 // source/full-frame bounds
  int _dw = 1, _dh = 1;               // grid dims (cells)
  const Palette *_pal = &PALETTES[0];
  std::vector<unsigned char> _cell;   // dw*dh palette indices
public:
  explicit DitherProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}

  void setSrcImg(OFX::Image *v) { _src = v; }

  // Build the grid from the whole source frame (single-threaded, called once).
  template <class PIX, int nComps, int maxv>
  void buildGrid(int paletteIdx, int pixel, float spread, float contrast) {
    _pal = &PALETTES[clampv(paletteIdx, 0, N_PALETTES - 1)];
    _bounds = _src->getBounds();
    const int W = _bounds.x2 - _bounds.x1, H = _bounds.y2 - _bounds.y1;
    if (W <= 0 || H <= 0) return;
    const int px = clampv(pixel, 1, 8);
    _dw = std::max(1, (int)std::lround((double)W / px));
    _dh = std::max(1, (int)std::lround((double)H / px));

    // Cell grid is stored top-down (row 0 = top of frame), matching the web
    // effect's ranges sy0 = floor(ly*H/dh) .. floor((ly+1)*H/dh). The inverse
    // of that partition for a top-down row ty is ((ty+1)*dh - 1) / H.
    std::vector<float> sum(_dw * _dh, 0.0f);
    std::vector<int>   cnt(_dw * _dh, 0);
    for (int y = 0; y < H; y++) {
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, _bounds.y1 + y);
      if (!row) continue;
      const int ty = H - 1 - y; // OFX bottom-up -> web top-down
      const int cy = std::min(_dh - 1, ((ty + 1) * _dh - 1) / H);
      for (int x = 0; x < W; x++) {
        PIX *p = row + x * nComps;
        // luma in 0..1 (sRGB-ish; see colour-space note in the plan)
        const float L = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / (float)maxv;
        const int cx = std::min(_dw - 1, ((x + 1) * _dw - 1) / W);
        const int ci = cy * _dw + cx;
        sum[ci] += L; cnt[ci]++;
      }
    }

    _cell.assign(_dw * _dh, 0);
    const int n = _pal->n;
    // ly is a top-down cell row here, so the Bayer index matches effect.js.
    for (int ly = 0; ly < _dh; ly++) {
      for (int lx = 0; lx < _dw; lx++) {
        const int ci = ly * _dw + lx;
        float L = cnt[ci] ? sum[ci] / cnt[ci] : 0.0f;
        L = clampv((L - 0.5f) * contrast + 0.5f, 0.0f, 1.0f);
        const float thr = bayer((ly & 3) * 4 + (lx & 3)) * spread;
        int idx = (int)std::lround(L * (n - 1) + thr);
        _cell[ci] = (unsigned char)clampv(idx, 0, n - 1);
      }
    }
  }

  // Per-pixel write: map output coord -> grid cell -> palette colour. The web
  // effect upscales the grid with canvas nearest-neighbour, which samples at
  // pixel CENTRES — floor((p + 0.5) * cells / size) — so we do the same.
  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &procWindow) {
    const int W = _bounds.x2 - _bounds.x1, H = _bounds.y2 - _bounds.y1;
    if (W <= 0 || H <= 0) return;
    for (int y = procWindow.y1; y < procWindow.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(procWindow.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - clampv(y - _bounds.y1, 0, H - 1);
      const int cy = std::min(_dh - 1, (2 * ty + 1) * _dh / (2 * H));
      for (int x = procWindow.x1; x < procWindow.x2; x++) {
        const int sx = clampv(x - _bounds.x1, 0, W - 1);
        const int cx = std::min(_dw - 1, (2 * sx + 1) * _dw / (2 * W));
        const float *c = _pal->rgb[_cell[cy * _dw + cx]];
        dst[0] = (PIX)(c[0] / 255.0f * maxv);
        dst[1] = (PIX)(c[1] / 255.0f * maxv);
        dst[2] = (PIX)(c[2] / 255.0f * maxv);
        if (nComps == 4) dst[3] = (PIX)maxv;
        dst += nComps;
      }
    }
  }

  // OFX entry: dispatched per depth via templates in the plugin's render().
  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class DitherPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_palette = nullptr;
  OFX::IntParam *_pixel = nullptr;
  OFX::DoubleParam *_spread = nullptr, *_contrast = nullptr;
public:
  explicit DitherPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _palette = fetchChoiceParam("palette");
    _pixel = fetchIntParam("pixel");
    _spread = fetchDoubleParam("spread");
    _contrast = fetchDoubleParam("contrast");
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

    int palette; _palette->getValueAtTime(args.time, palette);
    int pixel;   _pixel->getValueAtTime(args.time, pixel);
    // pixel size is in full-res pixels; scale for proxy/half-res renders
    pixel = std::max(1, (int)std::lround(pixel * args.renderScale.x));
    double spread, contrast;
    _spread->getValueAtTime(args.time, spread);
    _contrast->getValueAtTime(args.time, contrast);

    DitherProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                      \
    do {                                                                        \
      if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(palette, pixel, (float)spread,\
                       (float)contrast); proc.writePixels<PIX, 4, MAXV>(args.renderWindow); } \
      else         { proc.buildGrid<PIX, 3, MAXV>(palette, pixel, (float)spread,\
                       (float)contrast); proc.writePixels<PIX, 3, MAXV>(args.renderWindow); } \
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
mDeclarePluginFactory(DitherFactory, {}, {});

using namespace OFX;

void DitherFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Bayer Dither", "Bayer Dither", "Bayer Dither");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);          // grid spans the whole frame
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void DitherFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  ChoiceParamDescriptor *pal = desc.defineChoiceParam("palette");
  pal->setLabels("Palette", "Palette", "Palette");
  pal->appendOption("Game Boy");
  pal->appendOption("1-bit B/W");
  pal->appendOption("Amber CRT");
  pal->appendOption("Ice");
  pal->appendOption("Ember");
  pal->setDefault(0);

  IntParamDescriptor *px = desc.defineIntParam("pixel");
  px->setLabels("Pixel size", "Pixel size", "Pixel size");
  px->setRange(1, 8); px->setDisplayRange(1, 8); px->setDefault(2);

  DoubleParamDescriptor *sp = desc.defineDoubleParam("spread");
  sp->setLabels("Dither", "Dither", "Dither");
  sp->setRange(0, 1.5); sp->setDisplayRange(0, 1.5); sp->setDefault(1.0);

  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast");
  co->setRange(0.5, 2.0); co->setDisplayRange(0.5, 2.0); co->setDefault(1.0);
}

OFX::ImageEffect *DitherFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new DitherPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static DitherFactory p("org.purzos.bayerDither", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
