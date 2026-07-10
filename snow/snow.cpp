// Snow — analog TV snow / static: the fizzing speckle of an untuned channel.
// Each frame is a fresh field of deterministic hash noise, optionally chunked
// into bigger grains (`size`), monochrome or colour (`gray`), and — with
// `threshold` above zero — concentrated in the dark parts of the picture the way
// weak signal snows up the shadows first.
//
// Strictly per-pixel (own coords + hash) -> tiles ON. Deterministic.

#include "../common/purzfx.hpp"

using namespace purz;

class SnowProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0, size = 1, seed = 1; bool gray = true;
  float amount = 0.3f, threshold = 0.f, mix = 1.f;
  explicit SnowProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int gsz = std::max(1, size);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      const int cy = ty / gsz;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const int cx = (x - s.b.x1) / gsz;
        float sr, sg, sb;
        sr = hash3(cx, cy, frame * 7 + seed);
        if (gray) { sg = sb = sr; }
        else { sg = hash3(cx, cy, frame * 7 + seed + 101); sb = hash3(cx, cy, frame * 7 + seed + 202); }
        float gate = 1.f;
        if (threshold > 0.f) gate = smoothstep(threshold, 0.f, luma(c[0], c[1], c[2]));
        float a = clamp01(amount * gate);
        float o0 = lerp(c[0], sr, a), o1 = lerp(c[1], sg, a), o2 = lerp(c[2], sb, a);
        dst[0] = q<PIX, maxv>(lerp(c[0], o0, mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o1, mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o2, mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class SnowPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_threshold = nullptr, *_mix = nullptr;
  OFX::IntParam *_size = nullptr, *_seed = nullptr;
  OFX::BooleanParam *_gray = nullptr;
public:
  explicit SnowPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _size = fetchIntParam("size");
    _gray = fetchBooleanParam("gray"); _threshold = fetchDoubleParam("threshold");
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

    double amount, threshold, mix; int size, seed; bool gray;
    _amount->getValueAtTime(args.time, amount);
    _size->getValueAtTime(args.time, size);
    _gray->getValueAtTime(args.time, gray);
    _threshold->getValueAtTime(args.time, threshold);
    _seed->getValueAtTime(args.time, seed);
    _mix->getValueAtTime(args.time, mix);

    SnowProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time); proc.seed = seed; proc.gray = gray;
    proc.size = std::max(1, (int)std::floor(size * args.renderScale.x + 0.5));
    proc.amount = (float)amount; proc.threshold = (float)threshold; proc.mix = (float)mix;

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

mDeclarePluginFactory(SnowFactory, {}, {});
using namespace OFX;
void SnowFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Snow", true); }
void SnowFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.3);
  am->setHint("Snow overlay strength");
  IntParamDescriptor *sz = desc.defineIntParam("size");
  sz->setLabels("Grain Size", "Grain Size", "Grain Size"); sz->setRange(1, 6); sz->setDisplayRange(1, 6); sz->setDefault(1);
  sz->setHint("Speckle grain size in pixels");
  BooleanParamDescriptor *gr = desc.defineBooleanParam("gray");
  gr->setLabels("Gray", "Gray", "Gray"); gr->setDefault(true);
  gr->setHint("Monochrome speckle (off = coloured)");
  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold"); th->setRange(0, 1); th->setDisplayRange(0, 1); th->setDefault(0);
  th->setHint("Only snow where luma is below this (0 = everywhere)");
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *SnowFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new SnowPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static SnowFactory p("org.purzos.snow", 1, 0); ids.push_back(&p);
}
} }
