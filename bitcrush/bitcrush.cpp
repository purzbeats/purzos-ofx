// Bit Crush — bit-plane corruption. Each channel is masked down to its top
// `bits` planes (a hard posterize in integer space), then a random scatter of
// pixels get an XOR pattern stamped into the quantised byte — the stuck-bit
// look of a failing framebuffer. The corrupted set is re-rolled every `hold`
// frames so the noise crawls in bursts instead of buzzing.
//
// Strictly per-pixel -> tiles are safe. Deterministic: which pixels corrupt is
// a pure hash of coord + held state, never rand().

#include "../common/purzfx.hpp"

using namespace purz;

class BitCrushProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int bits = 5, xorv = 0, state = 0, H = 0;
  float prob = 0.05f, mix = 1.f;
  explicit BitCrushProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int keep = clampv(bits, 1, 8);
    const int mask = ~((1 << (8 - keep)) - 1) & 0xff;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int tx = x - s.b.x1;
        float c[4]; s.at(x, y, c);
        const bool corrupt = hash3(tx, ty, state * 131 + 1) < prob;
        float o[3];
        for (int k = 0; k < 3; k++) {
          int v = (int)std::floor(clamp01(c[k]) * 255.f + 0.5f) & mask;
          if (corrupt) v ^= xorv;
          o[k] = clampv(v, 0, 255) / 255.f;
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

class BitCrushPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_bits = nullptr, *_xor = nullptr, *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_prob = nullptr, *_mix = nullptr;
public:
  explicit BitCrushPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _bits = fetchIntParam("bits"); _xor = fetchIntParam("xor");
    _hold = fetchIntParam("hold"); _seed = fetchIntParam("seed");
    _prob = fetchDoubleParam("prob"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int bits, xorv, hold, seed; double prob, mix;
    _bits->getValueAtTime(args.time, bits);
    _xor->getValueAtTime(args.time, xorv);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _prob->getValueAtTime(args.time, prob);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    BitCrushProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.bits = bits; proc.xorv = xorv & 0xff;
    proc.prob = (float)prob; proc.mix = (float)mix;
    proc.state = frame / std::max(1, hold) + seed;
    proc.H = b.y2 - b.y1;

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

mDeclarePluginFactory(BitCrushFactory, {}, {});
using namespace OFX;
void BitCrushFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Bit Crush", true); }
void BitCrushFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, true);
  IntParamDescriptor *bi = desc.defineIntParam("bits");
  bi->setLabels("Bits", "Bits", "Bits"); bi->setRange(1, 8); bi->setDisplayRange(1, 8); bi->setDefault(5);
  IntParamDescriptor *xr = desc.defineIntParam("xor");
  xr->setLabels("XOR", "XOR", "XOR"); xr->setRange(0, 255); xr->setDisplayRange(0, 255); xr->setDefault(0);
  DoubleParamDescriptor *pr = desc.defineDoubleParam("prob");
  pr->setLabels("Prob", "Prob", "Prob"); pr->setRange(0, 1); pr->setDisplayRange(0, 1); pr->setDefault(0.05);
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 30); ho->setDisplayRange(1, 30); ho->setDefault(4);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BitCrushFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BitCrushPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BitCrushFactory p("org.purzos.bitCrush", 1, 0); ids.push_back(&p);
}
} }
