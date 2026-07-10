// Dropout — tape dropout dashes: where the oxide has flaked off the tape, a
// scanline briefly loses signal and reads as a bright white streak (with a thin
// black leading edge) riding along the line. Per pixel we hash a per-line dash
// cell against `density`; a hit paints the run white. `hold` freezes the dropout
// pattern for N frames so it doesn't strobe.
//
// Strictly per-pixel (own coords + hash) -> tiles ON. Deterministic.

#include "../common/purzfx.hpp"

using namespace purz;

class DropoutProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, state = 0, seed = 1;
  float density = 0.12f, length = 24.f, bright = 0.9f, mix = 1.f;
  explicit DropoutProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invL = 1.f / std::max(1e-3f, length);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float u = (float)(x - s.b.x1) * invL;
        int cell = (int)std::floor(u);
        float o0 = c[0], o1 = c[1], o2 = c[2];
        if (hash3(cell, ty, state * 11 + seed) < density) {   // this run drops out
          float pos = u - cell;                                // 0..1 within the dash
          float v = pos < 0.12f ? 0.f : bright;                // thin black leading edge, then white
          o0 = o1 = o2 = v;
        }
        dst[0] = q<PIX, maxv>(lerp(c[0], o0, mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o1, mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o2, mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class DropoutPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_density = nullptr, *_length = nullptr, *_bright = nullptr, *_mix = nullptr;
  OFX::IntParam *_hold = nullptr, *_seed = nullptr;
public:
  explicit DropoutPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _density = fetchDoubleParam("density"); _length = fetchDoubleParam("length");
    _bright = fetchDoubleParam("bright"); _hold = fetchIntParam("hold");
    _seed = fetchIntParam("seed"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    double density, length, bright, mix; int hold, seed;
    _density->getValueAtTime(args.time, density);
    _length->getValueAtTime(args.time, length);
    _bright->getValueAtTime(args.time, bright);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _mix->getValueAtTime(args.time, mix);

    DropoutProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    int frame = (int)std::floor(args.time);
    proc.state = frame / std::max(1, hold); proc.seed = seed;
    proc.density = (float)density; proc.bright = (float)bright; proc.mix = (float)mix;
    proc.length = (float)(length * args.renderScale.x);

#define GO(PIX, MAXV) do { if (nc == 4) proc.run<PIX, 4, MAXV>(args.renderWindow); \
                           else         proc.run<PIX, 3, MAXV>(args.renderWindow); } while (0)
    switch (depth) {
      case OFX::eBitDepthUByte:  GO(unsigned char, 255); break;
      case OFX::eBitDepthUShort: GO(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  GO(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef GO
  }
};

mDeclarePluginFactory(DropoutFactory, {}, {});
using namespace OFX;
void DropoutFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Dropout", true); }
void DropoutFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *de = desc.defineDoubleParam("density");
  de->setLabels("Density", "Density", "Density"); de->setRange(0, 1); de->setDisplayRange(0, 1); de->setDefault(0.12);
  de->setHint("How many dash cells drop out");
  DoubleParamDescriptor *le = desc.defineDoubleParam("length");
  le->setLabels("Length", "Length", "Length"); le->setRange(4, 80); le->setDisplayRange(4, 80); le->setDefault(24);
  le->setHint("Dash length in pixels");
  DoubleParamDescriptor *br = desc.defineDoubleParam("bright");
  br->setLabels("Bright", "Bright", "Bright"); br->setRange(0, 1); br->setDisplayRange(0, 1); br->setDefault(0.9);
  br->setHint("How white the dropout dash reads");
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 20); ho->setDisplayRange(1, 20); ho->setDefault(2);
  ho->setHint("Frames to freeze the dropout pattern");
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *DropoutFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new DropoutPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static DropoutFactory p("org.purzos.dropout", 1, 0); ids.push_back(&p);
}
} }
