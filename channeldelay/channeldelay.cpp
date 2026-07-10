// Channel Delay — spatial RGB split. The red and blue channels are pushed in
// opposite directions along a steerable vector (with an optional per-frame
// wobble that jitters the split over time), while green stays put — the
// chromatic tear of a misaligned analog signal.
//
// Reads each channel from a different offset -> needs neighbours, so tiles are
// off. Deterministic: the wobble is a hash of frame + seed, never rand().

#include "../common/purzfx.hpp"

using namespace purz;

class ChannelDelayProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float dirx = 0, diry = 0, jit = 0, mix = 1.f;
  explicit ChannelDelayProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float rC[4]; s.bilin(x + dirx + jit, y + diry + jit, rC);
        float bC[4]; s.bilin(x - dirx - jit, y - diry - jit, bC);
        float g[4];  s.at(x, y, g);
        float o[3] = { rC[0], g[1], bC[2] };
        dst[0] = q<PIX, maxv>(lerp(g[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(g[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(g[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(g[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ChannelDelayPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_seed = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_angle = nullptr, *_spread = nullptr, *_wobble = nullptr, *_mix = nullptr;
public:
  explicit ChannelDelayPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _seed = fetchIntParam("seed");
    _amount = fetchDoubleParam("amount"); _angle = fetchDoubleParam("angle");
    _spread = fetchDoubleParam("spread"); _wobble = fetchDoubleParam("wobble");
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

    int seed; double amount, angle, spread, wobble, mix;
    _seed->getValueAtTime(args.time, seed);
    _amount->getValueAtTime(args.time, amount);
    _angle->getValueAtTime(args.time, angle);
    _spread->getValueAtTime(args.time, spread);
    _wobble->getValueAtTime(args.time, wobble);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    const float rs = (float)args.renderScale.x;
    const float mag = (float)(amount * spread) * rs;
    ChannelDelayProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.dirx = std::cos((float)angle) * mag;
    proc.diry = std::sin((float)angle) * mag;
    proc.jit = (float)wobble * rs * sh2(frame, seed);
    proc.mix = (float)mix;

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

mDeclarePluginFactory(ChannelDelayFactory, {}, {});
using namespace OFX;
void ChannelDelayFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Channel Delay", false); }
void ChannelDelayFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 80); am->setDisplayRange(0, 80); am->setDefault(8);
  DoubleParamDescriptor *an = desc.defineDoubleParam("angle");
  an->setLabels("Angle", "Angle", "Angle"); an->setRange(-6.2832, 6.2832); an->setDisplayRange(-3.1416, 3.1416); an->setDefault(0.0);
  DoubleParamDescriptor *sp = desc.defineDoubleParam("spread");
  sp->setLabels("Spread", "Spread", "Spread"); sp->setRange(0, 2); sp->setDisplayRange(0, 2); sp->setDefault(1.0);
  DoubleParamDescriptor *wo = desc.defineDoubleParam("wobble");
  wo->setLabels("Wobble", "Wobble", "Wobble"); wo->setRange(0, 20); wo->setDisplayRange(0, 20); wo->setDefault(0.0);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ChannelDelayFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ChannelDelayPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ChannelDelayFactory p("org.purzos.channelDelay", 1, 0); ids.push_back(&p);
}
} }
