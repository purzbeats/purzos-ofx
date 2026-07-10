// Head Switch — the classic VHS bottom skew. Near the very bottom of the frame,
// where the tape's rotating heads switch, the picture tears sideways and dissolves
// into a band of head-switching noise. Rows deeper into the band tear harder and
// go noisier; the band height bobs slightly over time (drift).
//
// Reads a horizontally SHIFTED source (atWrap) -> tiles OFF. Deterministic:
// randomness is hash/vnoise on integer coords + seed + frame index.

#include "../common/purzfx.hpp"

using namespace purz;

class HeadSwitchProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0, seed = 1;
  float bandH = 18.f, skew = 40.f, noise = 0.4f, drift = 0.3f, mix = 1.f;
  explicit HeadSwitchProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    // band height bobs over time
    float bob = drift * bandH * std::sin((float)frame * 0.30f);
    float bh = std::max(1.f, bandH + bob);
    float bandTop = (H - 1) - bh;                 // top-down edge of the bottom band
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);        // top-down row
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float o[4] = { c[0], c[1], c[2], c[3] };
        if ((float)ty >= bandTop) {
          float t = clamp01(((float)ty - bandTop) / bh);   // 0 at band top .. 1 at very bottom
          float jit = 0.6f + 0.4f * vnoise1((float)ty * 0.7f + (float)frame * 3.f, seed);
          float tear = skew * t * jit;
          int sx = x + (int)std::floor(tear + 0.5f);
          float sm[4]; s.atWrap(sx, y, sm);
          float st = hash3(x - s.b.x1, ty, frame * 13 + seed);   // head-switch static
          float n = clamp01(noise * t);
          o[0] = lerp(sm[0], st, n);
          o[1] = lerp(sm[1], st, n);
          o[2] = lerp(sm[2], st, n);
          o[3] = sm[3];
        }
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(c[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class HeadSwitchPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_height = nullptr, *_skew = nullptr, *_noise = nullptr, *_drift = nullptr, *_mix = nullptr;
  OFX::IntParam *_seed = nullptr;
public:
  explicit HeadSwitchPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _height = fetchDoubleParam("height"); _skew = fetchDoubleParam("skew");
    _noise = fetchDoubleParam("noise"); _drift = fetchDoubleParam("drift");
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

    double height, skew, noise, drift, mix; int seed;
    _height->getValueAtTime(args.time, height);
    _skew->getValueAtTime(args.time, skew);
    _noise->getValueAtTime(args.time, noise);
    _drift->getValueAtTime(args.time, drift);
    _seed->getValueAtTime(args.time, seed);
    _mix->getValueAtTime(args.time, mix);

    HeadSwitchProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time); proc.seed = seed;
    proc.bandH = (float)(height * args.renderScale.x);
    proc.skew = (float)(skew * args.renderScale.x);
    proc.noise = (float)noise; proc.drift = (float)drift; proc.mix = (float)mix;

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

mDeclarePluginFactory(HeadSwitchFactory, {}, {});
using namespace OFX;
void HeadSwitchFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Head Switch", false); }
void HeadSwitchFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *he = desc.defineDoubleParam("height");
  he->setLabels("Height", "Height", "Height"); he->setRange(4, 60); he->setDisplayRange(4, 60); he->setDefault(18);
  he->setHint("Head-switch band height in pixels, measured from the bottom of the frame");
  DoubleParamDescriptor *sk = desc.defineDoubleParam("skew");
  sk->setLabels("Skew", "Skew", "Skew"); sk->setRange(0, 120); sk->setDisplayRange(0, 120); sk->setDefault(40);
  sk->setHint("Maximum horizontal tear in pixels at the very bottom");
  DoubleParamDescriptor *no = desc.defineDoubleParam("noise");
  no->setLabels("Noise", "Noise", "Noise"); no->setRange(0, 1); no->setDisplayRange(0, 1); no->setDefault(0.4);
  DoubleParamDescriptor *dr = desc.defineDoubleParam("drift");
  dr->setLabels("Drift", "Drift", "Drift"); dr->setRange(0, 1); dr->setDisplayRange(0, 1); dr->setDefault(0.3);
  dr->setHint("Time-varying bob of the band height");
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *HeadSwitchFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new HeadSwitchPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static HeadSwitchFactory p("org.purzos.headSwitch", 1, 0); ids.push_back(&p);
}
} }
