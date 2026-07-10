// Chromatic Aberration — radial lens RGB fringe. Red is sampled from slightly
// further out and blue from slightly further in along the vector from the frame
// centre, with the split growing toward the edges (a power falloff), so the
// channels separate at the corners the way a cheap lens disperses light. Green
// stays put; the three reads recombine into one pixel.
//
// A coordinate remap: reads the SOURCE with three bilinear samples. Needs the
// whole frame, so tiles are off. Deterministic (no time term).

#include "../common/purzfx.hpp"

using namespace purz;

class ChromaticAberrationProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, halfW = 1, amount = 0.02f, edgeBias = 2.f, mix = 1.f;
  explicit ChromaticAberrationProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invH = halfW > 0 ? 1.f / halfW : 0.f;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
        float r = std::sqrt(dx * dx + dy * dy) * invH;   // 0 centre .. ~1 edge
        float k = amount * std::pow(r, edgeBias);
        // red pushed out, blue pulled in, green centred
        float rr[4]; s.bilin(cx + dx * (1.f + k) - 0.5f, cy + dy * (1.f + k) - 0.5f, rr);
        float gg[4]; s.bilin(cx + dx - 0.5f, cy + dy - 0.5f, gg);
        float bb[4]; s.bilin(cx + dx * (1.f - k) - 0.5f, cy + dy * (1.f - k) - 0.5f, bb);
        float o[4]; o[0] = rr[0]; o[1] = gg[1]; o[2] = bb[2]; o[3] = gg[3];
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int j = 0; j < 4; j++) o[j] = lerp(src[j], o[j], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class ChromaticAberrationPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_edgeBias = nullptr, *_mix = nullptr;
public:
  explicit ChromaticAberrationPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _edgeBias = fetchDoubleParam("edgeBias");
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

    double amount, edgeBias, mix;
    _amount->getValueAtTime(args.time, amount);
    _edgeBias->getValueAtTime(args.time, edgeBias);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    ChromaticAberrationProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.halfW = (float)((b.x2 - b.x1) * 0.5);
    proc.amount = (float)amount; proc.edgeBias = (float)edgeBias; proc.mix = (float)mix;

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

mDeclarePluginFactory(ChromaticAberrationFactory, {}, {});
using namespace OFX;
void ChromaticAberrationFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Chromatic Aberration", false); }
void ChromaticAberrationFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 0.1); am->setDisplayRange(0, 0.1); am->setDefault(0.02);
  am->setHint("Radial RGB split as a fraction of half-width");
  DoubleParamDescriptor *eb = desc.defineDoubleParam("edgeBias");
  eb->setLabels("Edge Bias", "Edge Bias", "Edge Bias"); eb->setRange(0.5, 4); eb->setDisplayRange(0.5, 4); eb->setDefault(2.0);
  eb->setHint("Falloff power: higher concentrates the fringe at the edges");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *ChromaticAberrationFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new ChromaticAberrationPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static ChromaticAberrationFactory p("org.purzos.chromaticAberration", 1, 0); ids.push_back(&p);
}
} }
