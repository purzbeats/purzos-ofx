// Glyph Blocks — PETSCII-style 2x2 block-element mosaic, no font needed. Each
// cell is compared against its own four quadrant means: a quadrant lights up as
// "foreground" when it is brighter than the cell as a whole, so the frame turns
// into the half/quarter block glyphs of an 8-bit character ROM. Foreground is a
// chosen phosphor colour (or the cell's own average colour), background is a
// near-black.
//
// Whole-frame two-pass (build the cell glyphs, then write). Tiles OFF.
// Deterministic — a pure function of (source, params).

#include "../common/purzfx.hpp"
#include <vector>

using namespace purz;

namespace {
struct Scheme { const char *name; float rgb[3]; };
static const Scheme SCHEMES[] = {
  {"Green",    {0.00f, 1.00f, 0.27f}},
  {"Amber",    {1.00f, 0.69f, 0.00f}},
  {"White",    {0.92f, 0.92f, 0.96f}},
  {"C64 Blue", {0.33f, 0.33f, 0.80f}},
};
static const int N_SCHEMES = sizeof(SCHEMES) / sizeof(SCHEMES[0]);
} // namespace

class GlyphBlocksProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _b{};
  int _dw = 1, _dh = 1, _cell = 8;
  std::vector<unsigned char> _on;   // dw*dh : bit q set if quadrant q is foreground
  std::vector<float> _col;          // dw*dh*3 : cell average colour (normalised)
public:
  explicit GlyphBlocksProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void buildGrid(int cellPx, float contrast) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    _b = s.b; const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    _cell = clampv(cellPx, 4, 24);
    _dw = std::max(1, (W + _cell - 1) / _cell);
    _dh = std::max(1, (H + _cell - 1) / _cell);
    const int nc = _dw * _dh;
    std::vector<float> qL(nc * 4, 0.f);      // per-quadrant luma sum
    std::vector<int>   qN(nc * 4, 0);
    std::vector<float> cr(nc, 0.f), cg(nc, 0.f), cb(nc, 0.f);
    std::vector<int>   cn(nc, 0);
    const int half = _cell / 2;
    for (int y = 0; y < H; y++) {
      const int ty = H - 1 - y;                       // top-down
      const int cy = std::min(_dh - 1, ty / _cell);
      const int lty = ty - cy * _cell;
      const int qy = (lty >= half) ? 1 : 0;
      for (int x = 0; x < W; x++) {
        float c[4]; s.at(_b.x1 + x, _b.y1 + y, c);
        const int cx = std::min(_dw - 1, x / _cell);
        const int ltx = x - cx * _cell;
        const int qx = (ltx >= half) ? 1 : 0;
        const int ci = cy * _dw + cx;
        const int q = qy * 2 + qx;
        const float L = luma(c[0], c[1], c[2]);
        qL[ci * 4 + q] += L; qN[ci * 4 + q]++;
        cr[ci] += c[0]; cg[ci] += c[1]; cb[ci] += c[2]; cn[ci]++;
      }
    }
    _on.assign(nc, 0);
    _col.assign(nc * 3, 0.f);
    for (int i = 0; i < nc; i++) {
      const float inv = cn[i] ? 1.f / cn[i] : 0.f;
      _col[i * 3] = cr[i] * inv; _col[i * 3 + 1] = cg[i] * inv; _col[i * 3 + 2] = cb[i] * inv;
      float tot = 0.f; int totN = 0;
      for (int q = 0; q < 4; q++) { tot += qL[i * 4 + q]; totN += qN[i * 4 + q]; }
      const float cellMean = totN ? tot / totN : 0.f;
      unsigned char bits = 0;
      for (int q = 0; q < 4; q++) {
        const float qm = qN[i * 4 + q] ? qL[i * 4 + q] / qN[i * 4 + q] : 0.f;
        const float qmc = (qm - 0.5f) * contrast + 0.5f;   // contrast around mid
        if (qmc > cellMean) bits |= (unsigned char)(1u << q);
      }
      _on[i] = bits;
    }
  }

  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win, const float fg[3], bool useColor, float mix) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int W = s.W, H = s.H;
    if (W <= 0 || H <= 0) return;
    const float bg[3] = {0.02f, 0.02f, 0.03f};
    const int half = _cell / 2;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - clampv(y - _b.y1, 0, H - 1);
      const int cy = std::min(_dh - 1, ty / _cell);
      const int lty = ty - cy * _cell;
      const int qy = (lty >= half) ? 1 : 0;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int sx = clampv(x - _b.x1, 0, W - 1);
        const int cx = std::min(_dw - 1, sx / _cell);
        const int ltx = sx - cx * _cell;
        const int qx = (ltx >= half) ? 1 : 0;
        const int ci = cy * _dw + cx;
        const int qi = qy * 2 + qx;
        float src[4]; s.at(x, y, src);
        const bool foreground = (_on[ci] >> qi) & 1u;
        float o0, o1, o2;
        if (foreground) {
          if (useColor) { o0 = _col[ci * 3]; o1 = _col[ci * 3 + 1]; o2 = _col[ci * 3 + 2]; }
          else          { o0 = fg[0]; o1 = fg[1]; o2 = fg[2]; }
        } else { o0 = bg[0]; o1 = bg[1]; o2 = bg[2]; }
        dst[0] = q<PIX, maxv>(lerp(src[0], o0, mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], o1, mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], o2, mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(src[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class GlyphBlocksPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_cell = nullptr;
  OFX::ChoiceParam *_scheme = nullptr;
  OFX::BooleanParam *_color = nullptr;
  OFX::DoubleParam *_contrast = nullptr, *_mix = nullptr;
public:
  explicit GlyphBlocksPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _cell = fetchIntParam("cell"); _scheme = fetchChoiceParam("scheme");
    _color = fetchBooleanParam("color");
    _contrast = fetchDoubleParam("contrast"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int cell, scheme; bool useColor; double contrast, mix;
    _cell->getValueAtTime(args.time, cell);
    cell = std::max(1, (int)std::lround(cell * args.renderScale.x));
    _scheme->getValueAtTime(args.time, scheme);
    _color->getValueAtTime(args.time, useColor);
    _contrast->getValueAtTime(args.time, contrast);
    _mix->getValueAtTime(args.time, mix);
    const Scheme &S = SCHEMES[clampv(scheme, 0, N_SCHEMES - 1)];
    float fg[3] = {S.rgb[0], S.rgb[1], S.rgb[2]};

    GlyphBlocksProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define GO(PIX, MAXV) do { \
      if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(cell, (float)contrast); \
                     proc.writePixels<PIX, 4, MAXV>(args.renderWindow, fg, useColor, (float)mix); } \
      else         { proc.buildGrid<PIX, 3, MAXV>(cell, (float)contrast); \
                     proc.writePixels<PIX, 3, MAXV>(args.renderWindow, fg, useColor, (float)mix); } } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(GlyphBlocksFactory, {}, {});
using namespace OFX;
void GlyphBlocksFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Glyph Blocks", false); }
void GlyphBlocksFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  IntParamDescriptor *ce = desc.defineIntParam("cell");
  ce->setLabels("Cell size", "Cell size", "Cell size");
  ce->setRange(4, 24); ce->setDisplayRange(4, 24); ce->setDefault(8);
  ChoiceParamDescriptor *sc = desc.defineChoiceParam("scheme");
  sc->setLabels("Scheme", "Scheme", "Scheme");
  for (int i = 0; i < N_SCHEMES; i++) sc->appendOption(SCHEMES[i].name);
  sc->setDefault(0);
  BooleanParamDescriptor *co = desc.defineBooleanParam("color");
  co->setLabels("Source colour", "Source colour", "Source colour");
  co->setDefault(false); co->setHint("Ink each glyph with the cell's own average colour");
  DoubleParamDescriptor *ct = desc.defineDoubleParam("contrast");
  ct->setLabels("Contrast", "Contrast", "Contrast");
  ct->setRange(0.5, 2.5); ct->setDisplayRange(0.5, 2.5); ct->setDefault(1.2);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *GlyphBlocksFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new GlyphBlocksPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static GlyphBlocksFactory p("org.purzos.glyphBlocks", 1, 0); ids.push_back(&p);
}
} }
