// Data Bend — the look of editing raw pixel bytes in a hex editor. Each row is
// byte-rotated horizontally (wrapping around), with the rotation growing as you
// travel down the frame so the picture skews into a diagonal tear. A `corrupt`
// fraction of rows also cyclically rotate their channels (rgb -> gbr) for the
// smeared false-colour bands scrambled data produces. Row states hold for
// `hold` frames.
//
// Reads across the row (wrap sampling) -> needs neighbours, so tiles are off.
// Deterministic: the per-row corruption is a pure hash of row + held state.

#include "../common/purzfx.hpp"

using namespace purz;

class DataBendProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int state = 0, seed = 0, H = 0;
  float shift = 24.f, grow = 0.5f, corrupt = 0.15f, mix = 1.f;
  explicit DataBendProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      const int srow = (int)std::floor(shift + grow * ty + 0.5f);
      const bool rot = hash2(ty, state + seed) < corrupt;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float smp[4]; s.atWrap(x - srow, y, smp);
        float o[4];
        if (rot) { o[0] = smp[1]; o[1] = smp[2]; o[2] = smp[0]; } // rgb -> gbr
        else     { o[0] = smp[0]; o[1] = smp[1]; o[2] = smp[2]; }
        o[3] = smp[3];
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

class DataBendPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_shift = nullptr, *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_grow = nullptr, *_corrupt = nullptr, *_mix = nullptr;
public:
  explicit DataBendPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _shift = fetchIntParam("shift"); _hold = fetchIntParam("hold");
    _seed = fetchIntParam("seed");
    _grow = fetchDoubleParam("grow"); _corrupt = fetchDoubleParam("corrupt");
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

    int shift, hold, seed; double grow, corrupt, mix;
    _shift->getValueAtTime(args.time, shift);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _grow->getValueAtTime(args.time, grow);
    _corrupt->getValueAtTime(args.time, corrupt);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    DataBendProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.shift = (float)shift * (float)args.renderScale.x;
    proc.grow = (float)grow; proc.corrupt = (float)corrupt; proc.mix = (float)mix;
    proc.seed = seed;
    proc.state = frame / std::max(1, hold);
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

mDeclarePluginFactory(DataBendFactory, {}, {});
using namespace OFX;
void DataBendFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Data Bend", false); }
void DataBendFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  IntParamDescriptor *sh = desc.defineIntParam("shift");
  sh->setLabels("Shift", "Shift", "Shift"); sh->setRange(-200, 200); sh->setDisplayRange(-200, 200); sh->setDefault(24);
  DoubleParamDescriptor *gr = desc.defineDoubleParam("grow");
  gr->setLabels("Grow", "Grow", "Grow"); gr->setRange(-2, 2); gr->setDisplayRange(-2, 2); gr->setDefault(0.5);
  DoubleParamDescriptor *co = desc.defineDoubleParam("corrupt");
  co->setLabels("Corrupt", "Corrupt", "Corrupt"); co->setRange(0, 1); co->setDisplayRange(0, 1); co->setDefault(0.15);
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 30); ho->setDisplayRange(1, 30); ho->setDefault(5);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *DataBendFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new DataBendPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static DataBendFactory p("org.purzos.dataBend", 1, 0); ids.push_back(&p);
}
} }
