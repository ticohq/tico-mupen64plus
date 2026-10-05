#!/bin/bash
# Builds tico-mupen64plus.nro and tico-mupen64plus-module.zip (the module tico
# installs into sdmc:/tico/modules/). Uses a local devkitPro install when there
# is one, otherwise the switch-dev Docker image.
#
#   MESA_SDK_DIR        a Horizon Mesa SDK (lib/ and include/) to link instead of
#                       portlibs; needed for Zink, which portlibs' Mesa lacks.
#                       mesa-switch's unified configuration (NVK, plus EGL with
#                       nouveau and zink) installs one under
#                       mesa-unified-install*/opt/devkitpro/portlibs/switch
#   TICO_ENABLE_LOGGING 0 to build without sdmc:/tico/debug/mupen64plus.txt
#   BUILD_JOBS          parallel jobs (default: all cores)

SWITCH_DEV_IMAGE="${SWITCH_DEV_IMAGE:-ghcr.io/autorunhq/switch-dev:2026.10.05}"
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ ! -d /opt/devkitpro/devkitA64 ]; then
    MOUNTS=(-v "$ROOT_DIR:$ROOT_DIR")
    [ -n "$MESA_SDK_DIR" ] && MOUNTS+=(-v "$MESA_SDK_DIR:$MESA_SDK_DIR:ro")
    exec docker run --rm "${MOUNTS[@]}" -w "$ROOT_DIR" \
        -e MESA_SDK_DIR -e TICO_ENABLE_LOGGING -e BUILD_JOBS \
        "$SWITCH_DEV_IMAGE" bash "$ROOT_DIR/build_mupen64plus_nro.sh" "$@"
fi

set -e

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
source $DEVKITPRO/devkitA64/base_tools 2>/dev/null || true

PORTLIBS=$DEVKITPRO/portlibs/switch
LIBNX=$DEVKITPRO/libnx
MESA_SDK="${MESA_SDK_DIR:-$PORTLIBS}"
BUILD_DIR="$ROOT_DIR/build_tico"
TICO_DIR="$ROOT_DIR/tico"
JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# NACP version, and the version RetroAchievements sees in the User-Agent
APP_VERSION="3.0.0"

echo "=== Building tico-mupen64plus $APP_VERSION ==="

#------------------------------------------------------------------------------
# Step 1: mupen64plus, its plugins and tico/m64p
#------------------------------------------------------------------------------
echo "--- Step 1: libmupen64plus_tico.a ---"
cd "$ROOT_DIR"
make -j"$JOBS"
CORE_LIB="$ROOT_DIR/libmupen64plus_tico.a"

#------------------------------------------------------------------------------
# Step 2: glslang (compiles slang shaders to SPIR-V at runtime)
#------------------------------------------------------------------------------
# Kept outside build_tico, which is wiped every run: glslang only needs
# rebuilding when the submodule moves. Don't pass CMAKE_CXX_FLAGS here: it
# replaces the toolchain's -mtp=soft, and glslang's thread_locals then read a
# null thread pointer and crash on the first shader compile.
echo "--- Step 2: glslang ---"
if [ ! -f "$TICO_DIR/deps/glslang/CMakeLists.txt" ]; then
    git -C "$ROOT_DIR" submodule update --init tico/deps/glslang tico/deps/SPIRV-Reflect
fi
GLSLANG_BUILD="$ROOT_DIR/build_glslang_nx"
cmake -S "$TICO_DIR/deps/glslang" -B "$GLSLANG_BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
    -DBUILD_EXTERNAL=OFF -DENABLE_SPVREMAPPER=OFF -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_ENABLE_INSTALL=OFF > /dev/null
cmake --build "$GLSLANG_BUILD"
GLSLANG_LIBS=(
    "$GLSLANG_BUILD/glslang/libglslang.a"
    "$GLSLANG_BUILD/glslang/libglslang-default-resource-limits.a"
)

#------------------------------------------------------------------------------
# Step 3: NVK
#------------------------------------------------------------------------------
# NVK's loaderless libvulkan.a exports the vk* entry points as functions,
# which collide with volk's function pointers (paraLLEl-RDP and the frontend
# load through volk). Localize them; only vk_icd* is called from outside.
echo "--- Step 3: NVK ---"
NVK_SRC="$MESA_SDK/lib/libvulkan.a"
[ -f "$NVK_SRC" ] || { echo "Error: no NVK libvulkan.a in $MESA_SDK/lib"; exit 1; }
# one localized copy per SDK, so switching SDKs never reuses another's
NVK_CACHE="$ROOT_DIR/build_nvk/$(echo "$NVK_SRC" | md5sum | cut -c1-8)"
mkdir -p "$NVK_CACHE"
NVK_ARCHIVE="$NVK_CACHE/libvulkan_nvk.a"
if [ ! -f "$NVK_ARCHIVE" ] || [ "$NVK_SRC" -nt "$NVK_ARCHIVE" ]; then
    aarch64-none-elf-nm -g --defined-only "$NVK_SRC" 2>/dev/null \
        | awk '$3 ~ /^vk[A-Z]/ {print $3}' | sort -u > "$NVK_CACHE/libvulkan_nvk.syms"
    aarch64-none-elf-objcopy --localize-symbols="$NVK_CACHE/libvulkan_nvk.syms" "$NVK_SRC" "$NVK_ARCHIVE"
    # Mesa merges NVK's archives with the host ar, which leaves the Rust
    # members out of the symbol index; rebuild it with the devkitA64 archiver.
    aarch64-none-elf-ranlib "$NVK_ARCHIVE"
fi

#------------------------------------------------------------------------------
# Step 4: the frontend
#------------------------------------------------------------------------------
echo "--- Step 4: frontend ---"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CC="$DEVKITA64/bin/aarch64-none-elf-gcc"
CXX="$DEVKITA64/bin/aarch64-none-elf-g++"
PARALLEL_RDP="$ROOT_DIR/mupen64plus-video-paraLLEl/parallel-rdp"

COMMON_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O2 -g -fno-omit-frame-pointer"
COMMON_FLAGS="$COMMON_FLAGS -ffunction-sections -fdata-sections -D__SWITCH__ -DHAVE_LIBNX"
COMMON_FLAGS="$COMMON_FLAGS -DLIBARCHIVE_STATIC -DVK_USE_PLATFORM_VI_NN -DVK_NO_PROTOTYPES -DTICO_APP_VERSION=\"$APP_VERSION\""
COMMON_FLAGS="$COMMON_FLAGS -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS"
if [ "${TICO_ENABLE_LOGGING:-1}" -eq 0 ]; then
    COMMON_FLAGS="$COMMON_FLAGS -DDISABLE_LOGGING"
fi
COMMON_FLAGS="$COMMON_FLAGS -I$MESA_SDK/include -I$LIBNX/include -I$PORTLIBS/include -I$PORTLIBS/include/SDL2"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR -I$TICO_DIR/deps -I$TICO_DIR/deps/imgui -I$TICO_DIR/deps/imgui/backends"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR/deps/glslang -I$TICO_DIR/deps/SPIRV-Reflect"
COMMON_FLAGS="$COMMON_FLAGS -I$PARALLEL_RDP/volk -I$PARALLEL_RDP/vulkan-headers/include"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/custom -I$ROOT_DIR/mupen64plus-core/src -I$ROOT_DIR/mupen64plus-core/src/api -I$ROOT_DIR/mupen64plus-core/subprojects/md5"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/rcheevos/include -DRC_CLIENT_SUPPORTS_HASH"
COMMON_FLAGS="$COMMON_FLAGS -specs=$LIBNX/switch.specs"
CXXFLAGS="$COMMON_FLAGS -std=gnu++17 -fvisibility-inlines-hidden -fno-rtti -fno-exceptions"

TICO_SOURCES=(
    TicoMain.cpp TicoCore.cpp TicoRenderer.cpp TicoVulkan.cpp TicoGL.cpp
    TicoShaderChain.cpp TicoSlang.cpp UsbStorage.cpp TicoStubs.cpp
    overlay/imgui_overlay.cpp overlay/overlay_ui.cpp overlay/ra_alerts.cpp
    overlay/tico_config.cpp overlay/translation_manager.cpp
    deps/imgui/imgui.cpp deps/imgui/imgui_draw.cpp deps/imgui/imgui_tables.cpp
    deps/imgui/imgui_widgets.cpp deps/imgui/imgui_demo.cpp
    deps/imgui/backends/imgui_impl_vulkan.cpp
)
TICO_C_SOURCES=(glad.c deps/SPIRV-Reflect/spirv_reflect.c)
RCHEEVOS_SOURCES=($(find "$ROOT_DIR/rcheevos/src" -type f -name "*.c" ! -name "rc_client_external.c" ! -name "rc_libretro.c"))

OBJS=()
compile() { # compiler flags source object
    echo "  $(basename "$3")"
    $1 $2 -c "$3" -o "$4"
    OBJS+=("$4")
}
for src in "${TICO_SOURCES[@]}"; do
    compile "$CXX" "$CXXFLAGS" "$TICO_DIR/$src" "$BUILD_DIR/$(echo "${src%.cpp}" | tr / _).o"
done
# ImGui's GL backend loads GL through glad, like TicoGL
compile "$CXX" "$CXXFLAGS -DIMGUI_IMPL_OPENGL_LOADER_CUSTOM -include $TICO_DIR/glad.h" \
    "$TICO_DIR/deps/imgui/backends/imgui_impl_opengl3.cpp" "$BUILD_DIR/imgui_impl_opengl3.o"
for src in "${TICO_C_SOURCES[@]}"; do
    compile "$CC" "$COMMON_FLAGS -std=gnu11" "$TICO_DIR/$src" "$BUILD_DIR/$(basename "${src%.c}").o"
done
for src in "${RCHEEVOS_SOURCES[@]}"; do
    compile "$CC" "$COMMON_FLAGS -std=gnu11" "$src" \
        "$BUILD_DIR/rc_$(basename "$(dirname "$src")")_$(basename "${src%.c}").o"
done

#------------------------------------------------------------------------------
# Step 5: link
#------------------------------------------------------------------------------
echo "--- Step 5: link ---"
ELF="$BUILD_DIR/tico-mupen64plus.elf"
LINK_FLAGS="-specs=$LIBNX/switch.specs -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LINK_FLAGS="$LINK_FLAGS -Wl,--gc-sections -Wl,-Map=$BUILD_DIR/tico-mupen64plus.map"
LINK_FLAGS="$LINK_FLAGS -Wl,-u,vk_icdGetInstanceProcAddr -Wl,-u,vk_icdNegotiateLoaderICDInterfaceVersion"

# tico/deps/usbhsfs first: libusbhsfs (FAT/exFAT) that also reads NTFS through usbntfs.
# Mesa's EGL holds both GL drivers (NVC0, and Zink when the SDK has it).
LIBS="-L$TICO_DIR/deps/usbhsfs/lib -L$MESA_SDK/lib -L$PORTLIBS/lib -L$LIBNX/lib"
LIBS="$LIBS -lSDL2_mixer -lmpg123 -lmodplug -lopusfile -lopus -lvorbisidec -logg -lSDL2"
LIBS="$LIBS -lEGL -lGL -lglapi -lmesa_util_c11 -lblake3 -lmesa_util -lmesa_util_simd -lxmlconfig -lexpat"
[ -f "$MESA_SDK/lib/libdrm_nouveau.a" ] && LIBS="$LIBS -ldrm_nouveau"
LIBS="$LIBS -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -larchive -lbz2 -llzma -llz4 -lz -lzstd"
LIBS="$LIBS -lusbhsfs -lusbntfs -lnx -lm -lstdc++ -lpthread"

"$CXX" $LINK_FLAGS "${OBJS[@]}" "$CORE_LIB" "${GLSLANG_LIBS[@]}" \
    -Wl,--start-group "$NVK_ARCHIVE" $LIBS -Wl,--end-group -o "$ELF"

#------------------------------------------------------------------------------
# Step 6: NRO
#------------------------------------------------------------------------------
echo "--- Step 6: NRO ---"
NRO="$BUILD_DIR/tico-mupen64plus.nro"
NACP="$BUILD_DIR/tico-mupen64plus.nacp"
"$DEVKITPRO/tools/bin/nacptool" --create "tico Mupen64Plus" "ticoverse.com" "$APP_VERSION" "$NACP"

ROMFS="$BUILD_DIR/romfs"
mkdir -p "$ROMFS/module"
cp -R "$TICO_DIR/fonts" "$TICO_DIR/lang" "$TICO_DIR/assets" "$TICO_DIR/shaders" "$ROMFS/"
# the overlay builds its settings menu from the module's own definition
cp "$TICO_DIR/module/settings.json" "$ROMFS/module/"
"$DEVKITPRO/tools/bin/elf2nro" "$ELF" "$NRO" --nacp="$NACP" --romfsdir="$ROMFS"

#------------------------------------------------------------------------------
# Module bundle
#
# A module is a directory, not a bare NRO: tico discovers it by reading
# module.json, and everything the module owns -- its settings definition,
# gamelist and console artwork -- travels with it. The NRO sits beside
# module.json, so an installed bundle extracts straight into
# sdmc:/tico/modules/<id>/.
#------------------------------------------------------------------------------
MODULE_SRC="$TICO_DIR/module"
MODULE_ID=$(sed -n 's/.*"id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$MODULE_SRC/module.json" | head -1)
MODULE_OUT="$BUILD_DIR/module/$MODULE_ID"
mkdir -p "$MODULE_OUT"
cp -r "$MODULE_SRC/." "$MODULE_OUT/"
cp "$NRO" "$MODULE_OUT/"
# tico merges these into its own strings to label the settings screen
cp -R "$TICO_DIR/lang" "$MODULE_OUT/"
# Tico prefers .json.gz when resolving a gamelist.
if [ -d "$MODULE_OUT/gamelists" ]; then
    gzip -f -9 "$MODULE_OUT"/gamelists/*.json 2>/dev/null || true
fi
BUNDLE="$BUILD_DIR/tico-$MODULE_ID-module.zip"
( cd "$BUILD_DIR/module" && zip -qr "$BUNDLE" "$MODULE_ID" )

echo "======================================"
echo "Build successful!"
echo "  NRO:    $NRO ($(du -h "$NRO" | cut -f1))"
echo "  Module: $BUNDLE"
echo "          extracts to sdmc:/tico/modules/$MODULE_ID/"
echo "======================================"
