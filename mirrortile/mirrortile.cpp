// Mirror Tile — kaleidoscopic mirror tiling. The frame's normalised coordinate
// is scaled by `tiles` and folded with a triangle wave so every other tile is a
// mirror of its neighbour, giving a seamless mirrored repeat. `mode` picks which
// axes fold (the un-folded axis simply repeats). `zoom` scales the source read.
//
// A coordinate remap: read the SOURCE with a bilinear sample. Needs the whole
// frame, so tiles are off. Deterministic (no time term).

#include "../common/purzfx.hpp"

using namespace purz;

// ping-pong fold of u into [0,1]
static inline float mtFold(float u) {
  float m = wrapf(u, 2.f);
  return m > 1.f ? 2.f - m : m;
}

class MirrorTileProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float x1 = 0, y1 = 0, W = 1, H = 1, zoom = 1.f, mix = 1.f;
  int mode = 0, tiles = 2;
  explicit MirrorTileProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const bool foldX = (mode == 0 || mode == 2 || mode == 3);   // Mirror X / Quad / Both
    const bool foldY = (mode == 1 || mode == 2 || mode == 3);   // Mirror Y / Quad / Both
    const float invZ = zoom != 0 ? 1.f / zoom : 1.f;
    const int nt = tiles < 1 ? 1 : tiles;
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        // normalised, zoom around centre
        float u = ((x + 0.5f - x1) / W - 0.5f) * invZ + 0.5f;
        float v = ((y + 0.5f - y1) / H - 0.5f) * invZ + 0.5f;
        float tu = u * nt, tv = v * nt;
        float fu = foldX ? mtFold(tu) : wrapf(tu, 1.f);
        float fv = foldY ? mtFold(tv) : wrapf(tv, 1.f);
        float sxf = x1 + fu * W - 0.5f;
        float syf = y1 + fv * H - 0.5f;
        float o[4]; s.bilin(sxf, syf, o);
        if (mix < 1.f) { float src[4]; s.at(x, y, src); for (int k = 0; k < 4; k++) o[k] = lerp(src[k], o[k], mix); }
        dst[0] = q<PIX, maxv>(o[0]); dst[1] = q<PIX, maxv>(o[1]); dst[2] = q<PIX, maxv>(o[2]);
        if (nComps == 4) dst[3] = q<PIX, maxv>(o[3]);
      }
    }
  }
  void multiThreadProcessImages(OfxRectI) override {}
};

class MirrorTilePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_mode = nullptr;
  OFX::IntParam *_tiles = nullptr;
  OFX::DoubleParam *_zoom = nullptr, *_mix = nullptr;
public:
  explicit MirrorTilePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _mode = fetchChoiceParam("mode"); _tiles = fetchIntParam("tiles");
    _zoom = fetchDoubleParam("zoom"); _mix = fetchDoubleParam("mix");
  }
  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);
    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int mode, tiles; double zoom, mix;
    _mode->getValueAtTime(args.time, mode);
    _tiles->getValueAtTime(args.time, tiles);
    _zoom->getValueAtTime(args.time, zoom);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    MirrorTileProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.x1 = (float)b.x1; proc.y1 = (float)b.y1;
    proc.W = (float)(b.x2 - b.x1); proc.H = (float)(b.y2 - b.y1);
    proc.mode = mode; proc.tiles = std::max(1, tiles);
    proc.zoom = (float)zoom; proc.mix = (float)mix;

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

mDeclarePluginFactory(MirrorTileFactory, {}, {});
using namespace OFX;
void MirrorTileFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Mirror Tile", false); }
void MirrorTileFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  ChoiceParamDescriptor *md = desc.defineChoiceParam("mode");
  md->setLabels("Mode", "Mode", "Mode");
  md->appendOption("Mirror X"); md->appendOption("Mirror Y");
  md->appendOption("Quad Mirror"); md->appendOption("Mirror Both");
  md->setDefault(2);
  IntParamDescriptor *ti = desc.defineIntParam("tiles");
  ti->setLabels("Tiles", "Tiles", "Tiles"); ti->setRange(1, 8); ti->setDisplayRange(1, 8); ti->setDefault(2);
  DoubleParamDescriptor *zo = desc.defineDoubleParam("zoom");
  zo->setLabels("Zoom", "Zoom", "Zoom"); zo->setRange(0.3, 3); zo->setDisplayRange(0.3, 3); zo->setDefault(1.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *MirrorTileFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new MirrorTilePlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static MirrorTileFactory p("org.purzos.mirrorTile", 1, 0); ids.push_back(&p);
}
} }
