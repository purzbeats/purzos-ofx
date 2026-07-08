// Duotone — the whole image re-inked through a two-colour ramp: luma picks a
// point between the SHADOW ink and the HIGHLIGHT ink. Ships with a set of
// preset ink pairs; pick "Custom" and the two colour wells take over.
//
// Per-pixel tone mapping — no randomness, no neighbours, fully deterministic.
// Runs on the host's render thread.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdint>
#include <cmath>
#include <algorithm>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"

namespace {
template <class T> static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Duo { const char *name; float s[3], h[3]; }; // sRGB 0..255
static const Duo PRESETS[] = {
  {"Cyanotype",   {10, 30, 70},   {225, 240, 250}},
  {"Sepia",       {35, 20, 5},    {255, 235, 200}},
  {"Midnight",    {15, 10, 60},   {255, 60, 130}},   // navy -> hot pink
  {"Miami",       {45, 10, 90},   {60, 230, 240}},   // grape -> cyan
  {"Toxic",       {8, 12, 8},     {140, 255, 60}},
  {"Heat",        {20, 0, 5},     {255, 80, 40}},
  {"Gold Press",  {40, 15, 50},   {250, 200, 90}},
  {"Chrome",      {25, 35, 50},   {220, 230, 240}},
};
static const int N_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);
// choice index N_PRESETS == "Custom" -> the RGB params are used instead
} // namespace

// ---------------------------------------------------------------------------
class DuotoneProcessor : public OFX::ImageProcessor {
  OFX::Image *_src = nullptr;
  OfxRectI _bounds{};
  int _W = 0, _H = 0;
public:
  explicit DuotoneProcessor(OFX::ImageEffect &e) : OFX::ImageProcessor(e) {}
  void setSrcImg(OFX::Image *v) { _src = v; }

  template <class PIX, int nComps, int maxv>
  static inline PIX q(float v) {
    v = clampv(v, 0.0f, 1.0f);
    return maxv == 1 ? (PIX)v : (PIX)(v * maxv + 0.5f);
  }

  template <class PIX, int nComps, int maxv>
  void process(const OfxRectI &win, const float sh[3], const float hi[3],
               float contrast, float gamma, float mix) {
    _bounds = _src->getBounds();
    _W = _bounds.x2 - _bounds.x1; _H = _bounds.y2 - _bounds.y1;
    if (_W <= 0 || _H <= 0) return;

    for (int y = win.y1; y < win.y2; y++) {
      if (_effect.abort()) break;
      PIX *dst = (PIX *)_dstImg->getPixelAddress(win.x1, y);
      if (!dst) continue;
      PIX *row = (PIX *)_src->getPixelAddress(_bounds.x1, y);
      if (!row) { for (int x = win.x1; x < win.x2; x++, dst += nComps)
                    for (int c = 0; c < nComps; c++) dst[c] = 0; continue; }

      for (int x = win.x1; x < win.x2; x++, dst += nComps) {
        const int lx = clampv(x - _bounds.x1, 0, _W - 1);
        PIX *p = row + lx * nComps;
        const float r = p[0] / (float)maxv, g = p[1] / (float)maxv, b = p[2] / (float)maxv;
        float L = 0.299f * r + 0.587f * g + 0.114f * b;
        L = clampv((L - 0.5f) * contrast + 0.5f, 0.0f, 1.0f);
        L = std::pow(L, gamma);          // balance: where the crossover sits
        L = L * L * (3.0f - 2.0f * L);   // smoothstep keeps the ramp inky
        const float dr = sh[0] + (hi[0] - sh[0]) * L;
        const float dg = sh[1] + (hi[1] - sh[1]) * L;
        const float db = sh[2] + (hi[2] - sh[2]) * L;
        dst[0] = q<PIX, nComps, maxv>(r + (dr - r) * mix);
        dst[1] = q<PIX, nComps, maxv>(g + (dg - g) * mix);
        dst[2] = q<PIX, nComps, maxv>(b + (db - b) * mix);
        if (nComps == 4) dst[3] = p[3];
      }
    }
  }

  void multiThreadProcessImages(OfxRectI) override {}
};

// ---------------------------------------------------------------------------
class DuotonePlugin : public OFX::ImageEffect {
  OFX::Clip *_dst = nullptr, *_src = nullptr;
  OFX::ChoiceParam *_preset = nullptr;
  OFX::RGBParam *_shadow = nullptr, *_highlight = nullptr;
  OFX::DoubleParam *_contrast = nullptr, *_balance = nullptr, *_mix = nullptr;
public:
  explicit DuotonePlugin(OfxImageEffectHandle h) : OFX::ImageEffect(h) {
    _dst = fetchClip(kOfxImageEffectOutputClipName);
    _src = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _preset = fetchChoiceParam("preset");
    _shadow = fetchRGBParam("shadow");
    _highlight = fetchRGBParam("highlight");
    _contrast = fetchDoubleParam("contrast");
    _balance = fetchDoubleParam("balance");
    _mix = fetchDoubleParam("mix");
  }

  void render(const OFX::RenderArguments &args) override {
    std::unique_ptr<OFX::Image> dst(_dst->fetchImage(args.time));
    std::unique_ptr<OFX::Image> src(_src->fetchImage(args.time));
    if (!dst.get() || !src.get()) OFX::throwSuiteStatusException(kOfxStatFailed);

    OFX::BitDepthEnum depth = dst->getPixelDepth();
    OFX::PixelComponentEnum comps = dst->getPixelComponents();
    const int nc = (comps == OFX::ePixelComponentRGBA) ? 4
                 : (comps == OFX::ePixelComponentRGB) ? 3 : 0;
    if (!nc) OFX::throwSuiteStatusException(kOfxStatErrUnsupported);

    int preset; double contrast, balance, mix;
    _preset->getValueAtTime(args.time, preset);
    _contrast->getValueAtTime(args.time, contrast);
    _balance->getValueAtTime(args.time, balance);
    _mix->getValueAtTime(args.time, mix);

    float sh[3], hi[3];
    if (preset < N_PRESETS) {
      for (int c = 0; c < 3; c++) { sh[c] = PRESETS[preset].s[c] / 255.0f;
                                    hi[c] = PRESETS[preset].h[c] / 255.0f; }
    } else {
      double r, g, b;
      _shadow->getValueAtTime(args.time, r, g, b);
      sh[0] = (float)r; sh[1] = (float)g; sh[2] = (float)b;
      _highlight->getValueAtTime(args.time, r, g, b);
      hi[0] = (float)r; hi[1] = (float)g; hi[2] = (float)b;
    }
    const float gamma = (float)std::pow(2.0, -balance); // balance>0 lifts mids

    DuotoneProcessor proc(*this);
    proc.setSrcImg(src.get());
    proc.setDstImg(dst.get());
    proc.setRenderWindow(args.renderWindow);

#define DISPATCH(PIX, MAXV)                                                        \
    do {                                                                          \
      if (nc == 4) proc.process<PIX, 4, MAXV>(args.renderWindow, sh, hi,          \
                     (float)contrast, gamma, (float)mix);                         \
      else         proc.process<PIX, 3, MAXV>(args.renderWindow, sh, hi,          \
                     (float)contrast, gamma, (float)mix);                         \
    } while (0)

    switch (depth) {
      case OFX::eBitDepthUByte:  DISPATCH(unsigned char, 255); break;
      case OFX::eBitDepthUShort: DISPATCH(unsigned short, 65535); break;
      case OFX::eBitDepthFloat:  DISPATCH(float, 1); break;
      default: OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
#undef DISPATCH
  }
};

// ---------------------------------------------------------------------------
mDeclarePluginFactory(DuotoneFactory, {}, {});

using namespace OFX;

void DuotoneFactory::describe(OFX::ImageEffectDescriptor &desc) {
  desc.setLabels("Duotone", "Duotone", "Duotone");
  desc.setPluginGrouping("purzOS");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);           // strictly per-pixel — tiles are fine
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);
}

void DuotoneFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) {
  ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(ePixelComponentRGBA);
  src->addSupportedComponent(ePixelComponentRGB);
  src->setSupportsTiles(true);
  ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(ePixelComponentRGBA);
  dst->addSupportedComponent(ePixelComponentRGB);
  dst->setSupportsTiles(true);

  ChoiceParamDescriptor *pr = desc.defineChoiceParam("preset");
  pr->setLabels("Preset", "Preset", "Preset");
  for (int i = 0; i < N_PRESETS; i++) pr->appendOption(PRESETS[i].name);
  pr->appendOption("Custom");
  pr->setDefault(2); // Midnight

  RGBParamDescriptor *sh = desc.defineRGBParam("shadow");
  sh->setLabels("Shadow ink", "Shadow ink", "Shadow ink");
  sh->setDefault(0.06, 0.04, 0.24);
  sh->setHint("Used when Preset is Custom");

  RGBParamDescriptor *hi = desc.defineRGBParam("highlight");
  hi->setLabels("Highlight ink", "Highlight ink", "Highlight ink");
  hi->setDefault(1.0, 0.24, 0.51);
  hi->setHint("Used when Preset is Custom");

  DoubleParamDescriptor *co = desc.defineDoubleParam("contrast");
  co->setLabels("Contrast", "Contrast", "Contrast");
  co->setRange(0.25, 3); co->setDisplayRange(0.5, 2); co->setDefault(1.1);

  DoubleParamDescriptor *ba = desc.defineDoubleParam("balance");
  ba->setLabels("Balance", "Balance", "Balance");
  ba->setRange(-1, 1); ba->setDisplayRange(-1, 1); ba->setDefault(0);

  DoubleParamDescriptor *mi = desc.defineDoubleParam("mix");
  mi->setLabels("Mix", "Mix", "Mix");
  mi->setRange(0, 1); mi->setDisplayRange(0, 1); mi->setDefault(1);
}

OFX::ImageEffect *DuotoneFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) {
  return new DuotonePlugin(handle);
}

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray &ids) {
  static DuotoneFactory p("org.purzos.duotone", 1, 0);
  ids.push_back(&p);
}
} // namespace Plugin
} // namespace OFX
