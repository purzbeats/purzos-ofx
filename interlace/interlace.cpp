// Interlace — interlaced-field combing. The frame is split into fields of a few
// rows each; the odd field is sampled with a horizontal offset and darkened, so
// motion tears into the ragged comb of a 60i signal. With Motion on, the fields
// swap every frame (via frame parity) so the comb crawls.
//
// The odd field reads a horizontally shifted pixel (wrap) -> tiles OFF.
// Deterministic: the only time input is the integer frame index.

#include "../common/purzfx.hpp"

using namespace purz;

class InterlaceProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int shift = 6, thickness = 1, frame = 0, H = 0;
  bool motion = true;
  float darken = 0.3f, mix = 1.f;
  explicit InterlaceProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int th = std::max(1, thickness);
    const int par = motion ? frame : 0;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);                 // top-down
      const int f = (ty / th) + par;
      const int field = (((f % 2) + 2) % 2);
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        float o[4];
        if (field) {                                       // odd field: shift + darken
          s.atWrap(x + shift, y, o);
          for (int k = 0; k < 3; k++) o[k] *= (1.f - darken);
        } else {
          for (int k = 0; k < 4; k++) o[k] = c[k];
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

class InterlacePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_thickness = nullptr;
  OFX::DoubleParam *_darken = nullptr, *_shift = nullptr, *_mix = nullptr;
  OFX::BooleanParam *_motion = nullptr;
public:
  explicit InterlacePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _darken = fetchDoubleParam("darken"); _shift = fetchDoubleParam("shift");
    _thickness = fetchIntParam("thickness"); _motion = fetchBooleanParam("motion");
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

    int thickness; bool motion; double darken, shift, mix;
    _darken->getValueAtTime(args.time, darken);
    _shift->getValueAtTime(args.time, shift);
    _thickness->getValueAtTime(args.time, thickness);
    _motion->getValueAtTime(args.time, motion);
    _mix->getValueAtTime(args.time, mix);

    InterlaceProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.darken = (float)darken; proc.mix = (float)mix; proc.motion = motion;
    proc.shift = (int)std::lround(shift * args.renderScale.x);
    proc.thickness = std::max(1, (int)std::lround(thickness * args.renderScale.x));
    proc.frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds(); proc.H = b.y2 - b.y1;

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

mDeclarePluginFactory(InterlaceFactory, {}, {});
using namespace OFX;
void InterlaceFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Interlace", false); }
void InterlaceFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *da = desc.defineDoubleParam("darken");
  da->setLabels("Darken", "Darken", "Darken"); da->setRange(0, 1); da->setDisplayRange(0, 1); da->setDefault(0.3);
  DoubleParamDescriptor *sh = desc.defineDoubleParam("shift");
  sh->setLabels("Shift", "Shift", "Shift"); sh->setRange(0, 40); sh->setDisplayRange(0, 40); sh->setDefault(6.0);
  IntParamDescriptor *tk = desc.defineIntParam("thickness");
  tk->setLabels("Thickness", "Thickness", "Thickness"); tk->setRange(1, 4); tk->setDisplayRange(1, 4); tk->setDefault(1);
  BooleanParamDescriptor *mo = desc.defineBooleanParam("motion");
  mo->setLabels("Motion", "Motion", "Motion"); mo->setDefault(true);
  mo->setHint("Fields alternate each frame");
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *InterlaceFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new InterlacePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static InterlaceFactory p("org.purzos.interlace", 1, 0); ids.push_back(&p);
}
} }
