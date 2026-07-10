// Ghost — RF multipath ghosting. The signal arrives several times, each echo
// shifted further to the right and dimmer than the last (amplitude falls off by
// `decay` per bounce), with an optional bluish tint on the echoes to mimic a
// detuned analog receiver. Everything is summed and normalised so brightness
// holds steady.
//
// Reads horizontally offset neighbours -> tiles are off. Fully deterministic:
// no randomness at all, just a fixed sum of offset reads.

#include "../common/purzfx.hpp"

using namespace purz;

class GhostProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int count = 3;
  float offset = 14.f, decay = 0.55f, tint = 0.3f, mix = 1.f;
  explicit GhostProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int n = clampv(count, 1, 5);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float base[4]; s.at(x, y, base);
        float acc[3] = { base[0], base[1], base[2] };
        float wsum = 1.f;
        for (int i = 1; i <= n; i++) {
          float a = std::pow(decay, (float)i);
          float e[4]; s.bilin(x - i * offset, y, e);
          acc[0] += a * e[0] * (1.f - tint);            // echoes lose red -> bluish
          acc[1] += a * e[1];
          acc[2] += a * e[2];
          wsum += a;
        }
        float o[3] = { acc[0] / wsum, acc[1] / wsum, acc[2] / wsum };
        dst[0] = q<PIX, maxv>(lerp(base[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(base[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(base[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(base[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class GhostPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_count = nullptr;
  OFX::DoubleParam *_offset = nullptr, *_decay = nullptr, *_tint = nullptr, *_mix = nullptr;
public:
  explicit GhostPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _count = fetchIntParam("count");
    _offset = fetchDoubleParam("offset"); _decay = fetchDoubleParam("decay");
    _tint = fetchDoubleParam("tint"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int count; double offset, decay, tint, mix;
    _count->getValueAtTime(args.time, count);
    _offset->getValueAtTime(args.time, offset);
    _decay->getValueAtTime(args.time, decay);
    _tint->getValueAtTime(args.time, tint);
    _mix->getValueAtTime(args.time, mix);

    GhostProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.count = count;
    proc.offset = (float)offset * (float)args.renderScale.x;
    proc.decay = (float)decay; proc.tint = (float)tint; proc.mix = (float)mix;

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

mDeclarePluginFactory(GhostFactory, {}, {});
using namespace OFX;
void GhostFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Ghost", false); }
void GhostFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *of = desc.defineDoubleParam("offset");
  of->setLabels("Offset", "Offset", "Offset"); of->setRange(0, 120); of->setDisplayRange(0, 120); of->setDefault(14);
  IntParamDescriptor *cn = desc.defineIntParam("count");
  cn->setLabels("Count", "Count", "Count"); cn->setRange(1, 5); cn->setDisplayRange(1, 5); cn->setDefault(3);
  DoubleParamDescriptor *de = desc.defineDoubleParam("decay");
  de->setLabels("Decay", "Decay", "Decay"); de->setRange(0, 1); de->setDisplayRange(0, 1); de->setDefault(0.55);
  DoubleParamDescriptor *ti = desc.defineDoubleParam("tint");
  ti->setLabels("Tint", "Tint", "Tint"); ti->setRange(0, 1); ti->setDisplayRange(0, 1); ti->setDefault(0.3);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *GhostFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new GhostPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static GhostFactory p("org.purzos.ghost", 1, 0); ids.push_back(&p);
}
} }
