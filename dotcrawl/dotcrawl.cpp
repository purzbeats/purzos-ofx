// Dot Crawl — the shimmering checkerboard of crawling dots composite systems
// leave along sharp chroma edges, where luma and chroma bleed into each other.
// We detect a chroma change against a horizontal neighbour (2px over) and, where
// it's strong, overlay a subcarrier-frequency checkerboard that crawls upward
// each frame.
//
// Reads a neighbour pixel (x+2) -> tiles OFF. Deterministic (time enters only
// through the crawl phase, frame index).

#include "../common/purzfx.hpp"

using namespace purz;

namespace { const float PI = 3.14159265358979f; }

class DotCrawlProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int H = 0, frame = 0;
  float amount = 0.5f, freq = 2.f, speed = 1.f, edge = 0.5f, mix = 1.f;
  explicit DotCrawlProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float thr = lerp(0.5f, 0.02f, clamp01(edge));   // higher edge => more sensitive
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float c2[4]; s.at(x + 2, y, c2);
        float Y, I, Q, Y2, I2, Q2;
        rgb2yiq(c[0], c[1], c[2], Y, I, Q);
        rgb2yiq(c2[0], c2[1], c2[2], Y2, I2, Q2);
        float chromaDiff = std::fabs(I - I2) + std::fabs(Q - Q2);
        float e = smoothstep(0.f, thr, chromaDiff);
        float pat = std::sin(PI * freq * (x - s.b.x1) + PI * ty + speed * (float)frame * 0.5f);
        float m = amount * e * pat;
        Y = clamp01(Y + m * 0.30f);        // luma shimmer
        I += m * 0.20f; Q += m * 0.20f;    // chroma buzz
        float o[4];
        yiq2rgb(Y, I, Q, o[0], o[1], o[2]); o[3] = c[3];
        dst[0] = q<PIX, maxv>(lerp(c[0], o[0], mix));
        dst[1] = q<PIX, maxv>(lerp(c[1], o[1], mix));
        dst[2] = q<PIX, maxv>(lerp(c[2], o[2], mix));
        if (nComps == 4) dst[3] = q<PIX, maxv>(lerp(c[3], o[3], mix));
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class DotCrawlPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_freq = nullptr, *_speed = nullptr, *_edge = nullptr, *_mix = nullptr;
public:
  explicit DotCrawlPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _amount = fetchDoubleParam("amount"); _freq = fetchDoubleParam("freq");
    _speed = fetchDoubleParam("speed"); _edge = fetchDoubleParam("edge");
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

    double amount, freq, speed, edge, mix;
    _amount->getValueAtTime(args.time, amount);
    _freq->getValueAtTime(args.time, freq);
    _speed->getValueAtTime(args.time, speed);
    _edge->getValueAtTime(args.time, edge);
    _mix->getValueAtTime(args.time, mix);

    DotCrawlProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;
    proc.frame = (int)std::floor(args.time);
    proc.amount = (float)amount; proc.freq = (float)freq; proc.speed = (float)speed;
    proc.edge = (float)edge; proc.mix = (float)mix;

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

mDeclarePluginFactory(DotCrawlFactory, {}, {});
using namespace OFX;
void DotCrawlFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Dot Crawl", false); }
void DotCrawlFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 1); am->setDisplayRange(0, 1); am->setDefault(0.5);
  DoubleParamDescriptor *fr = desc.defineDoubleParam("freq");
  fr->setLabels("Frequency", "Frequency", "Frequency"); fr->setRange(0.5, 4); fr->setDisplayRange(0.5, 4); fr->setDefault(2.0);
  fr->setHint("Subcarrier frequency of the dot pattern");
  DoubleParamDescriptor *sp = desc.defineDoubleParam("speed");
  sp->setLabels("Speed", "Speed", "Speed"); sp->setRange(-4, 4); sp->setDisplayRange(-4, 4); sp->setDefault(1.0);
  sp->setHint("Crawl speed of the dots per frame");
  DoubleParamDescriptor *ed = desc.defineDoubleParam("edge");
  ed->setLabels("Edge", "Edge", "Edge"); ed->setRange(0, 1); ed->setDisplayRange(0, 1); ed->setDefault(0.5);
  ed->setHint("Chroma-edge sensitivity");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *DotCrawlFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new DotCrawlPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static DotCrawlFactory p("org.purzos.dotCrawl", 1, 0); ids.push_back(&p);
}
} }
