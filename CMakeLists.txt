cmake_minimum_required(VERSION 3.16)
project(purzos_ofx CXX)

# purzOS OFX plugins — native DaVinci Resolve effects ported from the web tools.
# Builds the vendored OFX C++ Support library once, then each plugin as an
# `.ofx` module bundled into <Name>.ofx.bundle/Contents/Win64/<Name>.ofx.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Plugins are shared modules; the static support lib linked into them must be
# position-independent or the GCC/Clang linker rejects it (R_X86_64_PC32 / -fPIC).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(OFX "${CMAKE_CURRENT_SOURCE_DIR}/openfx")
set(OFX_INCLUDES
  "${OFX}/include"
  "${OFX}/Support/include"
  "${OFX}/Support/Plugins/include")

# --- OFX C++ Support library (static) ---------------------------------------
file(GLOB SUPPORT_SRC "${OFX}/Support/Library/*.cpp")
add_library(ofxsupport STATIC ${SUPPORT_SRC})
target_include_directories(ofxsupport PUBLIC ${OFX_INCLUDES})
if(WIN32)
  target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32 _WINDOWS WINDOWS)
endif()

# --- arch subdir inside the bundle ------------------------------------------
if(WIN32)
  set(OFX_ARCH "Win64")
elseif(APPLE)
  set(OFX_ARCH "MacOS")
else()
  set(OFX_ARCH "Linux-x86-64")
endif()

# add_ofx_plugin(<TargetName> <source.cpp>...)
function(add_ofx_plugin name)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE ofxsupport)
  target_include_directories(${name} PRIVATE ${OFX_INCLUDES})
  if(WIN32)
    target_compile_definitions(${name} PRIVATE NOMINMAX WIN32 _WINDOWS WINDOWS)
  endif()
  set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX ".ofx")
  set(contents "${CMAKE_BINARY_DIR}/bundles/${name}.ofx.bundle/Contents")
  set(bundle "${contents}/${OFX_ARCH}")
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${bundle}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${name}>" "${bundle}/${name}.ofx"
    COMMENT "Bundling ${name}.ofx")

  # macOS bundles need an Info.plist or the host silently ignores them.
  if(APPLE)
    set(plist "${CMAKE_BINARY_DIR}/plists/${name}.Info.plist")
    file(WRITE "${plist}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
	<key>CFBundleName</key><string>${name}</string>
	<key>CFBundleExecutable</key><string>${name}.ofx</string>
	<key>CFBundleIdentifier</key><string>org.purzos.ofx.${name}</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
</dict>
</plist>
")
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy "${plist}" "${contents}/Info.plist"
      COMMENT "Adding Info.plist to ${name}.ofx.bundle")
  endif()
endfunction()

add_ofx_plugin(BayerDither "${CMAKE_CURRENT_SOURCE_DIR}/dither/dither.cpp")
add_ofx_plugin(Ascii "${CMAKE_CURRENT_SOURCE_DIR}/ascii/ascii.cpp")
add_ofx_plugin(PixelSort "${CMAKE_CURRENT_SOURCE_DIR}/pixelsort/pixelsort.cpp")

# glitch effects — OFX-native (no web counterpart yet)
add_ofx_plugin(SliceGlitch "${CMAKE_CURRENT_SOURCE_DIR}/sliceglitch/sliceglitch.cpp")
add_ofx_plugin(ChromaShift "${CMAKE_CURRENT_SOURCE_DIR}/chromashift/chromashift.cpp")

# the analog suite — 80s composite/VHS signal-path failures, one stage each.
# Suggested chain order: ChromaBleed -> LumaRing -> RainbowPhase -> TapeWow -> SyncDrift
add_ofx_plugin(ChromaBleed "${CMAKE_CURRENT_SOURCE_DIR}/chromableed/chromableed.cpp")
add_ofx_plugin(LumaRing "${CMAKE_CURRENT_SOURCE_DIR}/lumaring/lumaring.cpp")
add_ofx_plugin(RainbowPhase "${CMAKE_CURRENT_SOURCE_DIR}/rainbowphase/rainbowphase.cpp")
add_ofx_plugin(TapeWow "${CMAKE_CURRENT_SOURCE_DIR}/tapewow/tapewow.cpp")
add_ofx_plugin(SyncDrift "${CMAKE_CURRENT_SOURCE_DIR}/syncdrift/syncdrift.cpp")

# looks & weird optics — tone/glow/self-sampling effects
add_ofx_plugin(Halation "${CMAKE_CURRENT_SOURCE_DIR}/halation/halation.cpp")
add_ofx_plugin(Duotone "${CMAKE_CURRENT_SOURCE_DIR}/duotone/duotone.cpp")
add_ofx_plugin(VideoFeedback "${CMAKE_CURRENT_SOURCE_DIR}/feedback/feedback.cpp")
add_ofx_plugin(ScanWarp "${CMAKE_CURRENT_SOURCE_DIR}/scanwarp/scanwarp.cpp")
add_ofx_plugin(PhosphorMelt "${CMAKE_CURRENT_SOURCE_DIR}/phosphormelt/phosphormelt.cpp")
add_ofx_plugin(VectorRescan "${CMAKE_CURRENT_SOURCE_DIR}/vectorrescan/vectorrescan.cpp")

# ===========================================================================
# Expansion pack — pixelart / glitch / CRT / colour / warp. Each is a single
# self-contained .cpp sharing common/purzfx.hpp for the deterministic maths.
# ===========================================================================

# --- pixelart / quantize ---
add_ofx_plugin(Posterize "${CMAKE_CURRENT_SOURCE_DIR}/posterize/posterize.cpp")
add_ofx_plugin(ErrorDiffuse "${CMAKE_CURRENT_SOURCE_DIR}/errordiffuse/errordiffuse.cpp")
add_ofx_plugin(Mosaic       "${CMAKE_CURRENT_SOURCE_DIR}/mosaic/mosaic.cpp")
add_ofx_plugin(Halftone     "${CMAKE_CURRENT_SOURCE_DIR}/halftone/halftone.cpp")
add_ofx_plugin(Crosshatch   "${CMAKE_CURRENT_SOURCE_DIR}/crosshatch/crosshatch.cpp")
add_ofx_plugin(Threshold    "${CMAKE_CURRENT_SOURCE_DIR}/threshold/threshold.cpp")
add_ofx_plugin(GlyphBlocks  "${CMAKE_CURRENT_SOURCE_DIR}/glyphblocks/glyphblocks.cpp")

# --- glitch / datamosh ---
add_ofx_plugin(BlockMosh    "${CMAKE_CURRENT_SOURCE_DIR}/blockmosh/blockmosh.cpp")
add_ofx_plugin(BitCrush     "${CMAKE_CURRENT_SOURCE_DIR}/bitcrush/bitcrush.cpp")
add_ofx_plugin(BadSignal    "${CMAKE_CURRENT_SOURCE_DIR}/badsignal/badsignal.cpp")
add_ofx_plugin(LineStutter  "${CMAKE_CURRENT_SOURCE_DIR}/linestutter/linestutter.cpp")
add_ofx_plugin(ChannelDelay "${CMAKE_CURRENT_SOURCE_DIR}/channeldelay/channeldelay.cpp")
add_ofx_plugin(WaveShear    "${CMAKE_CURRENT_SOURCE_DIR}/waveshear/waveshear.cpp")
add_ofx_plugin(DataBend     "${CMAKE_CURRENT_SOURCE_DIR}/databend/databend.cpp")
add_ofx_plugin(Ghost        "${CMAKE_CURRENT_SOURCE_DIR}/ghost/ghost.cpp")

# --- colour / tone ---
add_ofx_plugin(Solarize     "${CMAKE_CURRENT_SOURCE_DIR}/solarize/solarize.cpp")
add_ofx_plugin(Thermal      "${CMAKE_CURRENT_SOURCE_DIR}/thermal/thermal.cpp")
add_ofx_plugin(GradientMap  "${CMAKE_CURRENT_SOURCE_DIR}/gradientmap/gradientmap.cpp")
add_ofx_plugin(NightVision  "${CMAKE_CURRENT_SOURCE_DIR}/nightvision/nightvision.cpp")
add_ofx_plugin(Chromatic    "${CMAKE_CURRENT_SOURCE_DIR}/chromatic/chromatic.cpp")
add_ofx_plugin(Bleach       "${CMAKE_CURRENT_SOURCE_DIR}/bleach/bleach.cpp")
add_ofx_plugin(Infrared     "${CMAKE_CURRENT_SOURCE_DIR}/infrared/infrared.cpp")
add_ofx_plugin(CrossProcess "${CMAKE_CURRENT_SOURCE_DIR}/crossprocess/crossprocess.cpp")

# --- CRT / display simulation ---
add_ofx_plugin(Scanlines   "${CMAKE_CURRENT_SOURCE_DIR}/scanlines/scanlines.cpp")
add_ofx_plugin(ShadowMask  "${CMAKE_CURRENT_SOURCE_DIR}/shadowmask/shadowmask.cpp")
add_ofx_plugin(CrtScreen   "${CMAKE_CURRENT_SOURCE_DIR}/crtscreen/crtscreen.cpp")
add_ofx_plugin(Bloom       "${CMAKE_CURRENT_SOURCE_DIR}/bloom/bloom.cpp")
add_ofx_plugin(Interlace   "${CMAKE_CURRENT_SOURCE_DIR}/interlace/interlace.cpp")
add_ofx_plugin(RollBar     "${CMAKE_CURRENT_SOURCE_DIR}/rollbar/rollbar.cpp")
add_ofx_plugin(Vignette    "${CMAKE_CURRENT_SOURCE_DIR}/vignette/vignette.cpp")
add_ofx_plugin(Overscan    "${CMAKE_CURRENT_SOURCE_DIR}/overscan/overscan.cpp")

# --- analog / VHS signal extras ---
add_ofx_plugin(HeadSwitch   "${CMAKE_CURRENT_SOURCE_DIR}/headswitch/headswitch.cpp")
add_ofx_plugin(DotCrawl     "${CMAKE_CURRENT_SOURCE_DIR}/dotcrawl/dotcrawl.cpp")
add_ofx_plugin(Hum          "${CMAKE_CURRENT_SOURCE_DIR}/hum/hum.cpp")
add_ofx_plugin(Snow         "${CMAKE_CURRENT_SOURCE_DIR}/snow/snow.cpp")
add_ofx_plugin(TrackingBar  "${CMAKE_CURRENT_SOURCE_DIR}/trackingbar/trackingbar.cpp")
add_ofx_plugin(ColorUnder   "${CMAKE_CURRENT_SOURCE_DIR}/colorunder/colorunder.cpp")
add_ofx_plugin(EdgeEnhance  "${CMAKE_CURRENT_SOURCE_DIR}/edgeenhance/edgeenhance.cpp")
add_ofx_plugin(Dropout      "${CMAKE_CURRENT_SOURCE_DIR}/dropout/dropout.cpp")

# --- optics / warp ---
add_ofx_plugin(Twirl "${CMAKE_CURRENT_SOURCE_DIR}/twirl/twirl.cpp")
add_ofx_plugin(Fisheye              "${CMAKE_CURRENT_SOURCE_DIR}/fisheye/fisheye.cpp")
add_ofx_plugin(Kaleidoscope         "${CMAKE_CURRENT_SOURCE_DIR}/kaleidoscope/kaleidoscope.cpp")
add_ofx_plugin(Bulge                "${CMAKE_CURRENT_SOURCE_DIR}/bulge/bulge.cpp")
add_ofx_plugin(MirrorTile           "${CMAKE_CURRENT_SOURCE_DIR}/mirrortile/mirrortile.cpp")
add_ofx_plugin(ChromaticAberration  "${CMAKE_CURRENT_SOURCE_DIR}/chromaticaberration/chromaticaberration.cpp")
add_ofx_plugin(Ripple               "${CMAKE_CURRENT_SOURCE_DIR}/ripple/ripple.cpp")
add_ofx_plugin(Melt                 "${CMAKE_CURRENT_SOURCE_DIR}/melt/melt.cpp")
add_ofx_plugin(Glass                "${CMAKE_CURRENT_SOURCE_DIR}/glass/glass.cpp")
