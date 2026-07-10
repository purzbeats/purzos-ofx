// Line Stutter — horizontal band shear. The frame is split into `bands`
// horizontal strips; each strip is yanked left/right by a hashed amount and
// wraps around the edges, so the picture judders in ragged blocks like a synced
// signal skipping. A `freeze` fraction of bands lock to a fixed state so some
// strips appear frozen while the rest keep stuttering. Shifts hold for `hold`
// frames.
//
// Reads across the row (wrap sampling) -> needs neighbours, so tiles are off.
// Deterministic: the per-band shift is a pure hash of band + held state.

#include "../common/purzfx.hpp"

using namespace purz;

class LineStutterProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int bands = 16, state = 0, seed = 0, H = 0, W = 0;
  float amount = 0.4f, freeze = 0.3f, mix = 1.f;
  explicit LineStutterProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int nb = std::max(2, bands);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      const int band = H > 0 ? (ty * nb) / H : 0;
      const int st = hash2(band, seed) < freeze ? 0 : state; // frozen bands hold
      const float h = hash2(band, st + seed);
      const int shift = (int)std::floor((h - 0.5f) * amount * (float)W + 0.5f);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float o[4]; s.atWrap(x + shift, y, o);
        float src[4]; s.at(x, y, src);
        dst[0] = q<PIX, maxv>(lerp(src[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(src[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(src[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(src[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class LineStutterPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_bands = nullptr, *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_freeze = nullptr, *_mix = nullptr;
public:
  explicit LineStutterPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _bands = fetchIntParam("bands"); _hold = fetchIntParam("hold");
    _seed = fetchIntParam("seed");
    _amount = fetchDoubleParam("amount"); _freeze = fetchDoubleParam("freeze");
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

    int bands, hold, seed; double amount, freeze, mix;
    _bands->getValueAtTime(args.time, bands);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _amount->getValueAtTime(args.time, amount);
    _freeze->getValueAtTime(args.time, freeze);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    LineStutterProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.bands = bands; proc.amount = (float)amount; proc.freeze = (float)freeze;
    proc.mix = (float)mix; proc.seed = seed;
    proc.state = frame / std::max(1, hold);
    proc.H = b.y2 - b.y1; proc.W = b.x2 - b.x1;

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

mDeclarePluginFactory(LineStutterFactory, {}, {});
using namespace OFX;
void LineStutterFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Line Stutter", false); }
void LineStutterFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.4);
  IntParamDescriptor *ba = desc.defineIntParam("bands");
  ba->setLabels("Bands", "Bands", "Bands"); ba->setRange(2, 64); ba->setDisplayRange(2, 64); ba->setDefault(16);
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 30); ho->setDisplayRange(1, 30); ho->setDefault(4);
  DoubleParamDescriptor *fr = desc.defineDoubleParam("freeze");
  fr->setLabels("Freeze", "Freeze", "Freeze"); fr->setRange(0, 1); fr->setDisplayRange(0, 1); fr->setDefault(0.3);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *LineStutterFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new LineStutterPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static LineStutterFactory p("org.purzos.lineStutter", 1, 0); ids.push_back(&p);
}
} }
