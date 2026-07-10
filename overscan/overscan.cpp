// Overscan — the rounded CRT bezel. The picture is zoomed slightly (the way a
// tube overscans past the visible raster) and then masked by a rounded-rectangle
// signed-distance field: everything outside the rounded frame is bezel colour,
// and a soft edge fades the picture into the bezel so the corners feel like real
// glass rather than a hard crop.
//
// Zooms the sampling coordinate + reads neighbours -> needs the whole frame,
// tiles OFF. Deterministic: time never enters the maths.

#include "../common/purzfx.hpp"

using namespace purz;

class OverscanProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  float cx = 0, cy = 0, halfW = 1, halfH = 1;
  float zoom = 1.05f, corner = 0.12f, edge = 0.06f, mix = 1.f;
  float bezel[3] = {0.f, 0.f, 0.f};
  explicit OverscanProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const float invZoom = zoom > 0.f ? 1.f / zoom : 1.f;
    const float ew = std::max(1e-4f, edge);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        float c[4]; s.at(x, y, c);
        const float nx = (x + 0.5f - cx) / halfW;
        const float ny = (y + 0.5f - cy) / halfH;
        // rounded-rectangle SDF in normalised [-1,1] space
        const float qx = std::fabs(nx) - (1.f - corner);
        const float qy = std::fabs(ny) - (1.f - corner);
        const float ax = std::max(qx, 0.f), ay = std::max(qy, 0.f);
        const float d = std::min(std::max(qx, qy), 0.f) + std::sqrt(ax * ax + ay * ay) - corner;
        const float fade = 1.f - smoothstep(-ew, 0.f, d);  // 1 inside, 0 in bezel
        float o[3];
        if (fade <= 0.f) {
          o[0] = bezel[0]; o[1] = bezel[1]; o[2] = bezel[2];
        } else {
          const float sx = cx + (x + 0.5f - cx) * invZoom - 0.5f;
          const float sy = cy + (y + 0.5f - cy) * invZoom - 0.5f;
          float samp[4]; s.bilin(sx, sy, samp);
          for (int k = 0; k < 3; k++) o[k] = lerp(bezel[k], samp[k], fade);
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

class OverscanPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::DoubleParam *_zoom = nullptr, *_corner = nullptr, *_edge = nullptr, *_mix = nullptr;
  OFX::RGBParam *_bezel = nullptr;
public:
  explicit OverscanPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _zoom = fetchDoubleParam("zoom"); _corner = fetchDoubleParam("corner");
    _edge = fetchDoubleParam("edge"); _bezel = fetchRGBParam("bezel");
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

    double zoom, corner, edge, mix, br, bg, bb;
    _zoom->getValueAtTime(args.time, zoom);
    _corner->getValueAtTime(args.time, corner);
    _edge->getValueAtTime(args.time, edge);
    _bezel->getValueAtTime(args.time, br, bg, bb);
    _mix->getValueAtTime(args.time, mix);

    OfxRectI b = src->getBounds();
    OverscanProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.zoom = (float)zoom; proc.corner = (float)corner; proc.edge = (float)edge; proc.mix = (float)mix;
    proc.bezel[0] = (float)br; proc.bezel[1] = (float)bg; proc.bezel[2] = (float)bb;
    proc.cx = 0.5f * (b.x1 + b.x2); proc.cy = 0.5f * (b.y1 + b.y2);
    proc.halfW = 0.5f * (b.x2 - b.x1); proc.halfH = 0.5f * (b.y2 - b.y1);

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

mDeclarePluginFactory(OverscanFactory, {}, {});
using namespace OFX;
void OverscanFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Overscan", false); }
void OverscanFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  DoubleParamDescriptor *zo = desc.defineDoubleParam("zoom");
  zo->setLabels("Zoom", "Zoom", "Zoom"); zo->setRange(1, 1.3); zo->setDisplayRange(1, 1.3); zo->setDefault(1.05);
  DoubleParamDescriptor *co = desc.defineDoubleParam("corner");
  co->setLabels("Corner", "Corner", "Corner"); co->setRange(0, 0.5); co->setDisplayRange(0, 0.5); co->setDefault(0.12);
  DoubleParamDescriptor *ed = desc.defineDoubleParam("edge");
  ed->setLabels("Edge", "Edge", "Edge"); ed->setRange(0, 0.3); ed->setDisplayRange(0, 0.3); ed->setDefault(0.06);
  RGBParamDescriptor *be = desc.defineRGBParam("bezel");
  be->setLabels("Bezel", "Bezel", "Bezel"); be->setDefault(0.0, 0.0, 0.0);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *OverscanFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new OverscanPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static OverscanFactory p("org.purzos.overscan", 1, 0); ids.push_back(&p);
}
} }
