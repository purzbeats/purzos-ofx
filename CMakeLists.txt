cmake_minimum_required(VERSION 3.16)
project(purzos_ofx CXX)

# purzOS OFX plugins — native DaVinci Resolve effects ported from the web tools.
# Builds the vendored OFX C++ Support library once, then each plugin as an
# `.ofx` module bundled into <Name>.ofx.bundle/Contents/Win64/<Name>.ofx.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
