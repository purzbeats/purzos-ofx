// Bloom — threshold glow. Bright pixels (luma above a threshold) are extracted
// into a scratch buffer, blurred with a separable box blur (horizontal then
// vertical pass, both edge-clamped running-sum), then added back on top of the
// original, tinted and scaled. The result is the soft phosphor bleed of an
// over-driven display.
//
// Whole-frame two-pass (build blurred buffer, then write) like dither.cpp, so it
// reads far beyond the tile -> tiles OFF. Deterministic: no randomness.

#include "../common/purzfx.hpp"
#include <vector>

using namespace purz;

class BloomProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _b{};
  int _W = 0, _H = 0;
  std::vector<float> _blur;                    // W*H*3, the blurred bright mask
public:
  int radius = 10;
  float threshold = 0.6f, intensity = 1.f, mix = 1.f;
  float tint[3] = {1.f, 1.f, 1.f};
  explicit BloomProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  // Pass 1: extract bright mask + separable box blur over the whole frame.
  template <class PIX, int nComps, int maxv>
  void buildGrid() {
    Src<PIX, nComps, maxv> s; s.init(_src);
    _b = s.b; _W = s.W; _H = s.H;
    if (_W <= 0 || _H <= 0) return;
    const int r = std::max(1, radius);
    const float w = (float)(2 * r + 1);

    std::vector<float> bright((size_t)_W * _H * 3, 0.f);
    for (int ly = 0; ly < _H; ly++) {
      for (int lx = 0; lx < _W; lx++) {
        float c[4]; s.at(_b.x1 + lx, _b.y1 + ly, c);
        const float e = std::max(0.f, luma(c[0], c[1], c[2]) - threshold);
        const size_t i = ((size_t)ly * _W + lx) * 3;
        bright[i] = e * c[0]; bright[i + 1] = e * c[1]; bright[i + 2] = e * c[2];
      }
    }

    std::vector<float> tmp((size_t)_W * _H * 3, 0.f);
    for (int ly = 0; ly < _H; ly++) {            // horizontal running-sum blur
      float acc[3] = {0.f, 0.f, 0.f};
      for (int k = -r; k <= r; k++) {
        const int xx = clampv(k, 0, _W - 1); const size_t i = ((size_t)ly * _W + xx) * 3;
        acc[0] += bright[i]; acc[1] += bright[i + 1]; acc[2] += bright[i + 2];
      }
      for (int lx = 0; lx < _W; lx++) {
        const size_t o = ((size_t)ly * _W + lx) * 3;
        tmp[o] = acc[0] / w; tmp[o + 1] = acc[1] / w; tmp[o + 2] = acc[2] / w;
        const int xr = clampv(lx - r, 0, _W - 1), xa = clampv(lx + r + 1, 0, _W - 1);
        const size_t ir = ((size_t)ly * _W + xr) * 3, ia = ((size_t)ly * _W + xa) * 3;
        acc[0] += bright[ia] - bright[ir]; acc[1] += bright[ia + 1] - bright[ir + 1];
        acc[2] += bright[ia + 2] - bright[ir + 2];
      }
    }

    _blur.assign((size_t)_W * _H * 3, 0.f);
    for (int lx = 0; lx < _W; lx++) {            // vertical running-sum blur
      float acc[3] = {0.f, 0.f, 0.f};
      for (int k = -r; k <= r; k++) {
        const int yy = clampv(k, 0, _H - 1); const size_t i = ((size_t)yy * _W + lx) * 3;
        acc[0] += tmp[i]; acc[1] += tmp[i + 1]; acc[2] += tmp[i + 2];
      }
      for (int ly = 0; ly < _H; ly++) {
        const size_t o = ((size_t)ly * _W + lx) * 3;
        _blur[o] = acc[0] / w; _blur[o + 1] = acc[1] / w; _blur[o + 2] = acc[2] / w;
        const int yr = clampv(ly - r, 0, _H - 1), ya = clampv(ly + r + 1, 0, _H - 1);
        const size_t ir = ((size_t)yr * _W + lx) * 3, ia = ((size_t)ya * _W + lx) * 3;
        acc[0] += tmp[ia] - tmp[ir]; acc[1] += tmp[ia + 1] - tmp[ir + 1];
        acc[2] += tmp[ia + 2] - tmp[ir + 2];
      }
    }
  }

  // Pass 2: source + intensity*tint*blur, blended back by mix.
  template <class PIX, int nComps, int maxv>
  void writePixels(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    if (_W <= 0 || _H <= 0) return;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ly = clampv(y - _b.y1, 0, _H - 1);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const int lx = clampv(x - _b.x1, 0, _W - 1);
        const size_t i = ((size_t)ly * _W + lx) * 3;
        float o[3];
        for (int k = 0; k < 3; k++) o[k] = c[k] + intensity * tint[k] * _blur[i + k];
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class BloomPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_radius = nullptr;
  OFX::DoubleParam *_threshold = nullptr, *_intensity = nullptr, *_mix = nullptr;
  OFX::RGBParam *_tint = nullptr;
public:
  explicit BloomPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _threshold = fetchDoubleParam("threshold"); _radius = fetchIntParam("radius");
    _intensity = fetchDoubleParam("intensity"); _tint = fetchRGBParam("tint");
    _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int radius; double threshold, intensity, mix, tr, tg, tb;
    _threshold->getValueAtTime(args.time, threshold);
    _radius->getValueAtTime(args.time, radius);
    _intensity->getValueAtTime(args.time, intensity);
    _tint->getValueAtTime(args.time, tr, tg, tb);
    _mix->getValueAtTime(args.time, mix);

    BloomProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.threshold = (float)threshold; proc.intensity = (float)intensity; proc.mix = (float)mix;
    proc.radius = std::max(1, (int)std::lround(radius * args.renderScale.x));
    proc.tint[0] = (float)tr; proc.tint[1] = (float)tg; proc.tint[2] = (float)tb;

#define GO(PIX, MAXV) do { if (nc == 4) { proc.buildGrid<PIX, 4, MAXV>(); proc.writePixels<PIX, 4, MAXV>(args.renderWindow); } \
                           else         { proc.buildGrid<PIX, 3, MAXV>(); proc.writePixels<PIX, 3, MAXV>(args.renderWindow); } } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(BloomFactory, {}, {});
using namespace OFX;
void BloomFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Bloom", false); }
void BloomFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold"); th->setRange(0, 1); th->setDisplayRange(0, 1); th->setDefault(0.6);
  IntParamDescriptor *ra = desc.defineIntParam("radius");
  ra->setLabels("Radius", "Radius", "Radius"); ra->setRange(1, 40); ra->setDisplayRange(1, 40); ra->setDefault(10);
  DoubleParamDescriptor *in = desc.defineDoubleParam("intensity");
  in->setLabels("Intensity", "Intensity", "Intensity"); in->setRange(0, 3); in->setDisplayRange(0, 3); in->setDefault(1.0);
  RGBParamDescriptor *ti = desc.defineRGBParam("tint");
  ti->setLabels("Tint", "Tint", "Tint"); ti->setDefault(1.0, 1.0, 1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BloomFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BloomPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BloomFactory p("org.purzos.bloom", 1, 0); ids.push_back(&p);
}
} }
