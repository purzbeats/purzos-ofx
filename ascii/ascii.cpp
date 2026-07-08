// ASCII — OFX port inspired by purzos/tools/effects/c64 (PETSCII converter),
// generalised to plain ASCII with an embedded public-domain 8x8 font so it has
// no font dependency and ships freely.
//
// Two modes (a `mode` param):
//   • Structural   — per cell, sample an 8x8 luma mask (threshold at the cell
//                    mean) and match the closest real glyph by Hamming distance
//                    (the C64 converter's trick). Edge/structure-aware ASCII.
//   • Brightness   — per cell, average luma -> a character from a density ramp.
//                    Classic tonal ASCII art.
//
// The character grid is derived from the OUTPUT aspect (square cells fill the
// frame), exactly like the web tool scaling its grid to the comp. Grid build
// and the per-pixel write both run on the host's render thread (we don't use
// the Support library's ImageProcessor threading); the grid is immutable once
// built.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"
#include "font8x8_basic.h"   // char font8x8_basic[128][8], rows top->bottom, bit0 = leftmost

namespace {

// Density ramp, densest -> lightest (Paul Bourke's 70-level). Indexed by (1-L).
static const char RAMP[] =
  "$@B%8&WM#*oahkbdpqwmZO0QLCJUYXzcvunxrjft/\\|()1{}[]?-_+~<>i!lI;:,\"^`'. ";

// Terminal colour schemes (sRGB 0..255). Background is always black.
struct Scheme { float r, g, b; };
static const Scheme SCHEMES[] = {
  {0, 255, 70},     // green
  {255, 176, 0},    // amber
  {235, 235, 245},  // white
};

template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline int popc(uint32_t v) {
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

// glyph pixel: column sx (0..7, left->right), row sy (0..7, top->bottom)
static inline int glyphBit(int code, int sx, int sy) {
  return (font8x8_basic[code & 127][sy] >> sx) & 1;
}

// Packed 8x8 glyph bitmaps for Hamming matching (bit p = sy*8 + sx).
struct GlyphTable { uint32_t hi[128], lo[128]; };
static const GlyphTable &glyphs() {
  static GlyphTable t = [] {
    GlyphTable g{};
    for (int code = 0; code < 128; code++) {
      uint32_t hi = 0, lo = 0;
      for (int p = 0; p < 64; p++) {
        if (glyphBit(code, p & 7, p >> 3)) { if (p < 32) hi |= 1u << p; else lo |= 1u << (p - 32); }
      }
      g.hi[code] = hi; g.lo[code] = lo;
    }
    return g;
  }();
  return t;
}

} // namespace

// ---------------------------------------------------------------------------
class AsciiProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _cols = 1, _rows = 1;
  float _cellW = 1, _cellH = 1;
  std::vector<unsigned char> _code;   // cols*rows chosen glyph codes
  std::vector<float> _fg;             // cols*rows * 3 foreground colour (0..1)
public:
  explicit AsciiProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void sample(int sx, int sy, float &L, float &r, float &g, float &b) {
    sx = clampv(sx, _bounds.x1, _bounds.x2 - 1);
    sy = clampv(sy, _bounds.y1, _bounds.y2 - 1);
    PIX *p = (PIX *)_src->getPixelAddress(sx, sy);
    if (!p) { L = r = g = b = 0; return; }
    r = (float)p[0] / maxv; g = (float)p[1] / maxv; b = (float)p[2] / maxv;
    L = 0.299f * r + 0.587f * g + 0.114f * b;
  }

  template <class PIX, int nComps, int maxv>
  void buildGrid(int mode, int cell, int scheme, float contrast) {
    _bounds = _src->getBounds();
    const int W = _bounds.x2 - _bounds.x1, H = _bounds.y2 - _bounds.y1;
    cell = clampv(cell, 2, 64);
    _cols = std::max(1, (int)std::lround((double)W / cell));
    _rows = std::max(1, (int)std::lround((double)H / cell));
    _cellW = (float)W / _cols; _cellH = (float)H / _rows;
    _code.assign(_cols * _rows, 32);
    _fg.assign(_cols * _rows * 3, 0.0f);

    const Scheme &sc = SCHEMES[clampv(scheme, 0, (int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])) - 1)];
    const bool srcColor = (scheme == 3);
    const GlyphTable &gt = glyphs();
    const int rampLen = (int)std::strlen(RAMP);

    for (int cy = 0; cy < _rows; cy++) {
      for (int cx = 0; cx < _cols; cx++) {
        float lum[64], sr = 0, sg = 0, sb = 0, meanL = 0;
        for (int syf = 0; syf < 8; syf++) {
          for (int sxi = 0; sxi < 8; sxi++) {
            // sample point: syf=0 is the TOP of the cell (OFX y is bottom-up)
            int sxp = _bounds.x1 + (int)((cx + (sxi + 0.5f) / 8.0f) * _cellW);
            int syp = _bounds.y1 + (int)((cy + 1) * _cellH - (syf + 0.5f) / 8.0f * _cellH);
            float L, r, g, b; sample<PIX, nComps, maxv>(sxp, syp, L, r, g, b);
            L = clampv((L - 0.5f) * contrast + 0.5f, 0.0f, 1.0f);
            lum[syf * 8 + sxi] = L; meanL += L; sr += r; sg += g; sb += b;
          }
        }
        meanL /= 64.0f; sr /= 64.0f; sg /= 64.0f; sb /= 64.0f;

        int code;
        if (mode == 0) { // structural: match mask to closest glyph
          uint32_t mhi = 0, mlo = 0;
          for (int p = 0; p < 64; p++)
            if (lum[p] > meanL) { if (p < 32) mhi |= 1u << p; else mlo |= 1u << (p - 32); }
          code = 32; int best = 1 << 30;
          for (int c = 32; c <= 126; c++) {
            int d = popc(gt.hi[c] ^ mhi) + popc(gt.lo[c] ^ mlo);
            if (d < best) { best = d; code = c; if (!d) break; }
          }
        } else {        // brightness ramp
          int idx = (int)std::lround((1.0f - meanL) * (rampLen - 1));
          code = (unsigned char)RAMP[clampv(idx, 0, rampLen - 1)];
        }

        const int ci = cy * _cols + cx;
        _code[ci] = (unsigned char)code;
        if (srcColor) { _fg[ci * 3] = sr; _fg[ci * 3 + 1] = sg; _fg[ci * 3 + 2] = sb; }
        else { _fg[ci * 3] = sc.r / 255.0f; _fg[ci * 3 + 1] = sc.g / 255.0f; _fg[ci * 3 + 2] = sc.b / 255.0f; }
      }
    }
  }

  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win) {
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const float fy = (float)(y - _bounds.y1);
      int cy = clampv((int)(fy / _cellH), 0, _rows - 1);
      float v = (fy - cy * _cellH) / _cellH;           // 0 bottom .. 1 top
      // glyph row (0 = top): band syf covers v in [1-(syf+1)/8, 1-syf/8), so
      // the inverse is 7 - floor(v*8) — NOT floor((1-v)*8), which is off by
      // one whenever v*8 lands on an integer (e.g. every row when cellH == 8).
      int syf = clampv(7 - (int)(v * 8.0f), 0, 7);
      for (int x = win.x1; x < win.x2; x++) {
        const float fx = (float)(x - _bounds.x1);
        int cx = clampv((int)(fx / _cellW), 0, _cols - 1);
        int sxi = clampv((int)((fx - cx * _cellW) / _cellW * 8.0f), 0, 7);
        const int ci = cy * _cols + cx;
        if (glyphBit(_code[ci], sxi, syf)) {
          dst[0] = (PIX)(_fg[ci * 3] * maxv);
          dst[1] = (PIX)(_fg[ci * 3 + 1] * maxv);
          dst[2] = (PIX)(_fg[ci * 3 + 2] * maxv);
        } else { dst[0] = dst[1] = dst[2] = (PIX)0; }
        if (nComps == 4) dst[3] = (PIX)maxv;
        dst += nComps;
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class AsciiPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_mode = nullptr, *_color = nullptr;
  OFX::IntParam *_cell = nullptr;
  OFX::DoubleParam *_contrast = nullptr;
public:
  explicit AsciiPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _mode = fetchChoiceParam("mode");
    _cell = fetchIntParam("cell");
    _color = fetchChoiceParam("color");
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

    int mode, cell, color; double contrast;
    _mode->getValueAtTime(args.time, mode);
    _cell->getValueAtTime(args.time, cell);
    _color->getValueAtTime(args.time, color);
    _contrast->getValueAtTime(args.time, contrast);
    // cell size is in full-res pixels; scale for proxy/half-res renders
    cell = std::max(2, (int)std::lround(cell * args.renderScale.x));

    AsciiProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(mode, cell, color, (float)contrast); \
                     proc.writePixels<PIX, 4, MAXV>(args.renderWindow); }         \
      else         { proc.buildGrid<PIX, 3, MAXV>(mode, cell, color, (float)contrast); \
                     proc.writePixels<PIX, 3, MAXV>(args.renderWindow); }         \
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
mDeclarePluginFactory(AsciiFactory, {}, {});

using namespace OFX;

void AsciiFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("ASCII", "ASCII", "ASCII");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(false);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void AsciiFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(false);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(false);

  ChoiceParamDescriptor *mode = desc.defineChoiceParam("mode");
  mode->setLabels("Mode", "Mode", "Mode");
  mode->appendOption("Structural");
  mode->appendOption("Brightness ramp");
  mode->setDefault(0);

  IntParamDescriptor *cell = desc.defineIntParam("cell");
  cell->setLabels("Cell size", "Cell size", "Cell size");
  cell->setRange(2, 64); cell->setDisplayRange(4, 32); cell->setDefault(10);

  ChoiceParamDescriptor *color = desc.defineChoiceParam("color");
  color->setLabels("Colour", "Colour", "Colour");
  color->appendOption("Green");
  color->appendOption("Amber");
  color->appendOption("White");
  color->appendOption("Source colour");
  color->setDefault(0);

  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast");
  co->setRange(0.5, 2.0); co->setDisplayRange(0.5, 2.0); co->setDefault(1.0);
}

OFX::ImageEffect *AsciiFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new AsciiPlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static AsciiFactory p("org.purzos.ascii", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
