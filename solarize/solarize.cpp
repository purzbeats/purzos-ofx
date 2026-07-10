// Solarize — the Sabattier effect: tones past a threshold flip toward their
// negative, so highlights (or, per channel, bright channels) invert while the
// shadows stay put. A smoothstep around the threshold keeps the crossover from
// hard-edging into a posterised band. Per-channel gives the classic false-colour
// solarisation; luma mode inverts the whole pixel on brightness.
//
// Strictly per-pixel colour map — no neighbours, no randomness. Tiles ON.

#include "../common/purzfx.hpp"

using namespace purz;

class SolarizeProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float threshold = 0.5f, amount = 1.f, smooth = 0.05f, mix = 1.f;
  bool perChannel = true;
  explicit SolarizeProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float e0 = threshold - smooth, e1 = threshold + smooth;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float o[3];
        if (perChannel) {
          for (int k = 0; k < 3; k++) {
            float w = smoothstep(e0, e1, c[k]) * amount;
            o[k] = lerp(c[k], 1.f - c[k], w);
          }
        } else {
          float L = luma(c[0], c[1], c[2]);
          float w = smoothstep(e0, e1, L) * amount;
          for (int k = 0; k < 3; k++) o[k] = lerp(c[k], 1.f - c[k], w);
        }
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(c[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class SolarizePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_threshold = nullptr, *_amount = nullptr, *_smooth = nullptr, *_mix = nullptr;
  OFX::BooleanParam *_perChannel = nullptr;
public:
  explicit SolarizePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _threshold = fetchDoubleParam("threshold"); _amount = fetchDoubleParam("amount");
    _smooth = fetchDoubleParam("smooth"); _perChannel = fetchBooleanParam("perChannel");
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

    double threshold, amount, smooth, mix; bool perChannel;
    _threshold->getValueAtTime(args.time, threshold);
    _amount->getValueAtTime(args.time, amount);
    _smooth->getValueAtTime(args.time, smooth);
    _perChannel->getValueAtTime(args.time, perChannel);
    _mix->getValueAtTime(args.time, mix);

    SolarizeProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.threshold = (float)threshold; proc.amount = (float)amount;
    proc.smooth = (float)smooth; proc.perChannel = perChannel; proc.mix = (float)mix;

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

mDeclarePluginFactory(SolarizeFactory, {}, {});
using namespace OFX;
void SolarizeFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Solarize", true); }
void SolarizeFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  DoubleParamDescriptor *th = desc.defineDoubleParam("threshold");
  th->setLabels("Threshold", "Threshold", "Threshold"); th->setRange(0, 1); th->setDisplayRange(0, 1); th->setDefault(0.5);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(1.0);
  am->setHint("Inversion strength past the threshold");
  DoubleParamDescriptor *sm = desc.defineDoubleParam("smooth");
  sm->setLabels("Smooth", "Smooth", "Smooth"); sm->setRange(0, 0.5); sm->setDisplayRange(0, 0.5); sm->setDefault(0.05);
  BooleanParamDescriptor *pc = desc.defineBooleanParam("perChannel");
  pc->setLabels("Per channel", "Per channel", "Per channel"); pc->setDefault(true);
  pc->setHint("Invert each channel independently (false colour); off inverts on luma");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *SolarizeFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new SolarizePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static SolarizeFactory p("org.purzos.solarize", 1, 0); ids.push_back(&p);
}
} }
