// Melt — per-column downward drip, a liquify / pixel-melt. Every column gets a
// drip length from 1-D value noise (so neighbouring columns sag by different
// amounts), and the sampling row is pulled from above by that length, dragging
// the image downward like running paint. `speed` animates the drip and `bias`
// shapes how many columns sag hard.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (value noise + integer frame index).

#include "../common/purzfx.hpp"

using namespace purz;

class MeltProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float x1 = 0, amount = 80.f, noiseScale = 6.f, drift = 0.f, bias = 0.5f, mix = 1.f;
  explicit MeltProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invNS = noiseScale > 0 ? 1.f / noiseScale : 0.f;
    const float expo = lerp(0.4f, 2.5f, clamp01(bias));   // bias shapes the distribution
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float base = vnoise1((x - x1) * invNS + drift, 7);   // 0..1 per column
        float drip = amount * std::pow(clamp01(base), expo);
        // bottom-up coords: sampling at y+drip pulls content DOWN visually
        float sxf = (x + 0.5f) - 0.5f;
        float syf = (y + 0.5f) + drip - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class MeltPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_noiseScale = nullptr, *_speed = nullptr,
                   *_bias = nullptr, *_mix = nullptr;
public:
  explicit MeltPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _noiseScale = fetchDoubleParam("noiseScale");
    _speed = fetchDoubleParam("speed"); _bias = fetchDoubleParam("bias");
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

    double amount, noiseScale, speed, bias, mix;
    _amount->getValueAtTime(args.time, amount);
    _noiseScale->getValueAtTime(args.time, noiseScale);
    _speed->getValueAtTime(args.time, speed);
    _bias->getValueAtTime(args.time, bias);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    const int frame = (int)std::floor(args.time);
    MeltProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.x1 = (float)b.x1;
    proc.amount = (float)(amount * args.renderScale.x);   // max drip in px (renderScale-scaled)
    proc.noiseScale = (float)noiseScale;
    proc.drift = (float)(speed * frame * 0.1);
    proc.bias = (float)bias; proc.mix = (float)mix;

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

mDeclarePluginFactory(MeltFactory, {}, {});
using namespace OFX;
void MeltFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Melt", false); }
void MeltFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 300); am->setDisplayRange(0, 300); am->setDefault(80.0);
  am->setHint("Maximum drip length in pixels");
  DoubleParamDescriptor *ns = desc.defineDoubleParam("noiseScale");
  ns->setLabels("Noise Scale", "Noise Scale", "Noise Scale"); ns->setRange(1, 40); ns->setDisplayRange(1, 40); ns->setDefault(6.0);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-4, 4); sp->setDisplayRange(-4, 4); sp->setDefault(0.4);
  sp->setHint("Time-driven drip motion (sampled at the integer frame)");
  DoubleParamDescriptor *bi = desc.defineDoubleParam("bias");
  bi->setLabels("Bias", "Bias", "Bias"); bi->setRange(0, 1); bi->setDisplayRange(0, 1); bi->setDefault(0.5);
  bi->setHint("Shapes which columns drip most");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *MeltFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new MeltPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static MeltFactory p("org.purzos.melt", 1, 0); ids.push_back(&p);
}
} }
