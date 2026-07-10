// Block Mosh — fake datamosh. The frame is diced into square blocks; each block
// is shoved along a per-block hashed vector (held for N frames so the smear
// lurches rather than shimmers) and its pixels are read from that displaced
// spot, then blended toward a longer "smear" read so motion trails the way a
// corrupted P-frame drags macroblocks across the picture.
//
// A coordinate remap that reaches across block widths -> needs neighbours, so
// tiles are off. Deterministic: the displacement is a pure hash of block +
// held state (frame / hold), never rand().

#include "../common/purzfx.hpp"

using namespace purz;

class BlockMoshProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
public:
  int block = 16, state = 0, H = 0;
  float amount = 1.2f, decay = 0.6f, mix = 1.f;
  explicit BlockMoshProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  void run(const OfxRectI &win) {
    Src<PIX, nComps, maxv> s; s.init(_src);
    const int blk = std::max(1, block);
    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      const int ty = H - 1 - (y - s.b.y1);              // top-down
      const int by = ty / blk;
      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int tx = x - s.b.x1;
        const int bx = tx / blk;
        float ox = amount * blk * sh3(bx, by, state);
        float oy = amount * blk * sh3(by, bx, state + 7);
        float disp[4]; s.bilin(x + ox, y + oy, disp);   // displaced block read
        float smear[4]; s.bilin(x + ox * 1.6f, y + oy * 1.6f, smear); // longer trail
        float o[4];
        for (int k = 0; k < 4; k++) o[k] = lerp(disp[k], smear[k], decay);
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

class BlockMoshPlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::IntParam *_block = nullptr, *_hold = nullptr, *_seed = nullptr;
  OFX::DoubleParam *_amount = nullptr, *_decay = nullptr, *_mix = nullptr;
public:
  explicit BlockMoshPlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _block = fetchIntParam("block"); _hold = fetchIntParam("hold");
    _seed = fetchIntParam("seed");
    _amount = fetchDoubleParam("amount"); _decay = fetchDoubleParam("decay");
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

    int block, hold, seed; double amount, decay, mix;
    _block->getValueAtTime(args.time, block);
    _hold->getValueAtTime(args.time, hold);
    _seed->getValueAtTime(args.time, seed);
    _amount->getValueAtTime(args.time, amount);
    _decay->getValueAtTime(args.time, decay);
    _mix->getValueAtTime(args.time, mix);

    const int frame = (int)std::floor(args.time);
    OfxRectI b = src->getBounds();
    BlockMoshProcessor proc(*this);
    proc.setSrcImg(src.get()); proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);
    proc.block = std::max(1, (int)(block * args.renderScale.x + 0.5));
    proc.amount = (float)amount;
    proc.decay = (float)decay;
    proc.mix = (float)mix;
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

mDeclarePluginFactory(BlockMoshFactory, {}, {});
using namespace OFX;
void BlockMoshFactory::describe(OFX::ImageEffectDescriptor &desc) { purz::describeStd(desc, "Block Mosh", false); }
void BlockMoshFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  purz::defineStdClips(desc, false);
  IntParamDescriptor *bl = desc.defineIntParam("block");
  bl->setLabels("Block", "Block", "Block"); bl->setRange(4, 64); bl->setDisplayRange(4, 64); bl->setDefault(16);
  DoubleParamDescriptor *am = desc.defineDoubleParam("amount");
  am->setLabels("Amount", "Amount", "Amount"); am->setRange(0, 4); am->setDisplayRange(0, 4); am->setDefault(1.2);
  DoubleParamDescriptor *de = desc.defineDoubleParam("decay");
  de->setLabels("Decay", "Decay", "Decay"); de->setRange(0, 1); de->setDisplayRange(0, 1); de->setDefault(0.6);
  IntParamDescriptor *ho = desc.defineIntParam("hold");
  ho->setLabels("Hold", "Hold", "Hold"); ho->setRange(1, 30); ho->setDisplayRange(1, 30); ho->setDefault(6);
  IntParamDescriptor *se = desc.defineIntParam("seed");
  se->setLabels("Seed", "Seed", "Seed"); se->setRange(0, 9999); se->setDisplayRange(0, 9999); se->setDefault(1);
  DoubleParamDescriptor *mx = desc.defineDoubleParam("mix");
  mx->setLabels("Mix", "Mix", "Mix"); mx->setRange(0, 1); mx->setDisplayRange(0, 1); mx->setDefault(1.0);
}
OFX::ImageEffect *BlockMoshFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new BlockMoshPlugin(handle);
}
namespace OFX { namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static BlockMoshFactory p("org.purzos.blockMosh", 1, 0); ids.push_back(&p);
}
} }
