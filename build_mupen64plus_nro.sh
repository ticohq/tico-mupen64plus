#!/bin/bash

export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITPPC=$DEVKITPRO/devkitPPC
export DEVKITA64=$DEVKITPRO/devkitA64

echo "=== Building Mupen64Plus NRO with Tico Overlay ==="

# Include devkitA64 toolchain
source $DEVKITPRO/devkitA64/base_tools 2>/dev/null || true

PORTLIBS=$DEVKITPRO/portlibs/switch
LIBNX=$DEVKITPRO/libnx
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"
SWITCH_VULKAN_LIBRARY="${SWITCH_VULKAN_LIBRARY:-}"

# Project root
ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Standalone mode (TICO_STANDALONE=1, see STANDALONE_PLAN.md): no libretro
# frame pump — emulator free-runs on a pthread and presents from the VI path.
TICO_STANDALONE="${TICO_STANDALONE:-0}"
if [ "$TICO_STANDALONE" -eq 1 ]; then
    BUILD_DIR="$ROOT_DIR/build_tico_standalone"
else
    BUILD_DIR="$ROOT_DIR/build_tico"
fi
TICO_DIR="$ROOT_DIR/tico"

# Prefer a locally built Mesa tree when present so the NRO doesn't keep
# embedding the older portlibs OpenGL stack.
MESA_SOURCE_ROOT="${MESA_SOURCE_ROOT:-}"
MESA_BUILD_ROOT=""

if [ -z "$MESA_SOURCE_ROOT" ]; then
    for candidate in "$HOME/mesa-clean" "/mesa-clean"; do
        if [ -d "$candidate" ]; then
            MESA_SOURCE_ROOT="$candidate"
            break
        fi
    done
fi

if [ -n "$MESA_SOURCE_ROOT" ]; then
    if [ -f "$MESA_SOURCE_ROOT/build/src/egl/libEGL.a" ]; then
        MESA_BUILD_ROOT="$MESA_SOURCE_ROOT/build"
    elif [ -f "$MESA_SOURCE_ROOT/src/egl/libEGL.a" ]; then
        MESA_BUILD_ROOT="$MESA_SOURCE_ROOT"
    fi
fi

USE_CUSTOM_MESA=0
MESA_ARCHIVES=()
USE_VULKAN_FRONTEND=1
VULKAN_ARCHIVES=()

if [ -z "$SWITCH_VULKAN_LIBRARY" ]; then
    if [ -f "/opt/nvk-switch/lib/libvulkan.a" ]; then
        SWITCH_VULKAN_LIBRARY="/opt/nvk-switch/lib/libvulkan.a"
    elif [ -f "$MESA_NVK_DIR/src/nouveau/vulkan/libvulkan.a" ]; then
        SWITCH_VULKAN_LIBRARY="$MESA_NVK_DIR/src/nouveau/vulkan/libvulkan.a"
    elif [ -n "$MESA_BUILD_ROOT" ] && [ -f "$MESA_BUILD_ROOT/src/nouveau/vulkan/libvulkan.a" ]; then
        SWITCH_VULKAN_LIBRARY="$MESA_BUILD_ROOT/src/nouveau/vulkan/libvulkan.a"
    fi
fi

if [ -n "$SWITCH_VULKAN_LIBRARY" ] && [ -f "$SWITCH_VULKAN_LIBRARY" ]; then
    VULKAN_ARCHIVES=("$SWITCH_VULKAN_LIBRARY")
    echo "Using Switch Vulkan archive: $SWITCH_VULKAN_LIBRARY"
else
    echo "Error: Switch Vulkan archive not found."
    echo "Set SWITCH_VULKAN_LIBRARY or mount Mesa NVK at $MESA_NVK_DIR."
    exit 1
fi

if [ "$USE_VULKAN_FRONTEND" -eq 0 ] && [ -n "$MESA_BUILD_ROOT" ]; then
    REQUIRED_MESA_ARCHIVES=(
        "$MESA_BUILD_ROOT/src/egl/libEGL.a"
        "$MESA_BUILD_ROOT/src/mapi/shared-glapi/libglapi.a"
        "$MESA_BUILD_ROOT/src/gallium/drivers/nouveau/libnouveau.a"
        "$MESA_BUILD_ROOT/src/nouveau/codegen/libnouveau_codegen.a"
        "$MESA_BUILD_ROOT/src/gallium/winsys/nouveau/switch/libnouveauwinsys.a"
        "$MESA_BUILD_ROOT/libdrm_nouveau/lib/libdrm_nouveau.a"
    )

    MISSING_MESA_ARCHIVE=0
    for archive in "${REQUIRED_MESA_ARCHIVES[@]}"; do
        if [ ! -f "$archive" ]; then
            MISSING_MESA_ARCHIVE=1
            echo "Custom Mesa archive missing: $archive"
        fi
    done

    if [ "$MISSING_MESA_ARCHIVE" -eq 0 ]; then
        USE_CUSTOM_MESA=1
        MESA_ARCHIVES=("${REQUIRED_MESA_ARCHIVES[@]}")
        echo "Using custom Mesa build from: $MESA_BUILD_ROOT"
    else
        echo "Falling back to devkitPro portlibs Mesa."
    fi
else
    if [ -n "$MESA_SOURCE_ROOT" ]; then
        echo "Custom Mesa not found at $MESA_SOURCE_ROOT, using devkitPro portlibs Mesa."
    else
        echo "Custom Mesa not found, using devkitPro portlibs Mesa."
    fi
fi

# ============================================================
# Step 1: Build mupen64plus as a static library (.a)
# ============================================================
echo "--- Step 1: Building mupen64plus static library ---"

cd "$ROOT_DIR"
# make clean 2>/dev/null || true
echo "Building libnx core with Parallel RDP/RSP + LLE enabled"
BUILD_JOBS="${BUILD_JOBS:-$(command -v nproc >/dev/null && nproc || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# Core objects are built in-tree with mode-dependent defines; force a clean
# when switching between libretro-pump and standalone builds.
MODE_STAMP="$ROOT_DIR/.tico_build_mode"
BUILD_MODE="libretro"
[ "$TICO_STANDALONE" -eq 1 ] && BUILD_MODE="standalone"
if [ -f "$MODE_STAMP" ] && [ "$(cat "$MODE_STAMP")" != "$BUILD_MODE" ]; then
    echo "Build mode changed ($(cat "$MODE_STAMP") -> $BUILD_MODE): make clean"
    make clean >/dev/null 2>&1 || true
fi
echo "$BUILD_MODE" > "$MODE_STAMP"

MAKE_ARGS=(platform=libnx)
[ "$TICO_STANDALONE" -eq 1 ] && MAKE_ARGS+=(TICO_STANDALONE=1)
make -j"$BUILD_JOBS" "${MAKE_ARGS[@]}"

STATIC_LIB="$ROOT_DIR/mupen64plus_next_libretro_libnx.a"
if [ ! -f "$STATIC_LIB" ]; then
    echo "Error: Static library not found at $STATIC_LIB"
    exit 1
fi
echo "Static library built: $STATIC_LIB"

# ============================================================
# Step 2: Compile Tico overlay sources
# ============================================================
echo "--- Step 2: Compiling Tico overlay sources ---"

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

CC="${DEVKITA64}/bin/aarch64-none-elf-gcc"
CXX="${DEVKITA64}/bin/aarch64-none-elf-g++"
AR="${DEVKITA64}/bin/aarch64-none-elf-ar"

COMMON_FLAGS="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -O0 -g"
COMMON_FLAGS="$COMMON_FLAGS -ffunction-sections -fdata-sections -D__SWITCH__ -DHAVE_LIBNX"
COMMON_FLAGS="$COMMON_FLAGS -fno-lto"
if [ "${TICO_ENABLE_LOGGING:-1}" -eq 0 ]; then
    COMMON_FLAGS="$COMMON_FLAGS -DDISABLE_LOGGING"
fi
COMMON_FLAGS="$COMMON_FLAGS -DTICO_VULKAN_OVERLAY -DHAVE_VULKAN -DVK_USE_PLATFORM_VI_NN -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS"
if [ "$TICO_STANDALONE" -eq 1 ]; then
    COMMON_FLAGS="$COMMON_FLAGS -DTICO_STANDALONE"
fi
COMMON_FLAGS="$COMMON_FLAGS -I$LIBNX/include -I$PORTLIBS/include -I$PORTLIBS/include/SDL2"
COMMON_FLAGS="$COMMON_FLAGS -I$TICO_DIR -I$TICO_DIR/deps"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/libretro-common/include"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/mupen64plus-core/src"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/mupen64plus-core/subprojects/md5"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/libretro"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/mupen64plus-video-paraLLEl/parallel-rdp/volk"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/mupen64plus-video-paraLLEl/parallel-rdp/vulkan-headers/include"
COMMON_FLAGS="$COMMON_FLAGS -I$ROOT_DIR/rcheevos/include -DRC_CLIENT_SUPPORTS_HASH"
COMMON_FLAGS="$COMMON_FLAGS -specs=$LIBNX/switch.specs"

if [ "$USE_CUSTOM_MESA" -eq 1 ]; then
    COMMON_FLAGS="$COMMON_FLAGS -I$MESA_SOURCE_ROOT/include -I$MESA_SOURCE_ROOT/libdrm_nouveau/include"
fi

CXXFLAGS="$COMMON_FLAGS -std=gnu++17 -fvisibility-inlines-hidden -fno-rtti -fno-exceptions"

# Tico C++ sources
TICO_SOURCES=(
    "$TICO_DIR/TicoMain.cpp"
    "$TICO_DIR/TicoCore.cpp"
    "$TICO_DIR/TicoOverlay.cpp"
    "$TICO_DIR/TicoVulkan.cpp"
    "$TICO_DIR/TicoTranslationManager.cpp"
    "$TICO_DIR/TicoStubs.cpp"
)

TICO_C_SOURCES=()

# ImGui sources (from tico/deps/imgui)
IMGUI_DIR="$TICO_DIR/deps/imgui"
IMGUI_SOURCES=(
    "$IMGUI_DIR/imgui.cpp"
    "$IMGUI_DIR/imgui_draw.cpp"
    "$IMGUI_DIR/imgui_tables.cpp"
    "$IMGUI_DIR/imgui_widgets.cpp"
    "$IMGUI_DIR/imgui_demo.cpp"
    "$IMGUI_DIR/backends/imgui_impl_vulkan.cpp"
)

IMGUI_FLAGS="-I$IMGUI_DIR -I$IMGUI_DIR/backends"

# rcheevos sources
RCHEEVOS_DIR="$ROOT_DIR/rcheevos"
RCHEEVOS_SOURCES=($(find "$RCHEEVOS_DIR/src" -type f -name "*.c" ! -name "rc_client_external.c" 2>/dev/null || true))

TICO_OBJS=()

# Compile Tico C++ sources
for src in "${TICO_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile glad.c
for src in "${TICO_C_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.c}.o)"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile rcheevos sources
for src in "${RCHEEVOS_SOURCES[@]}"; do
    filename=$(basename "$src")
    dirprefix=$(basename $(dirname "$src"))
    obj="$BUILD_DIR/rc_${dirprefix}_${filename%.c}.o"
    echo "  CC  $src"
    $CC $COMMON_FLAGS -std=gnu11 -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

# Compile ImGui sources
for src in "${IMGUI_SOURCES[@]}"; do
    obj="$BUILD_DIR/$(basename ${src%.cpp}.o)"
    echo "  CXX $src"
    $CXX $CXXFLAGS $IMGUI_FLAGS -c "$src" -o "$obj"
    if [ $? -ne 0 ]; then
        echo "Error compiling $src"
        exit 1
    fi
    TICO_OBJS+=("$obj")
done

echo "Compiled ${#TICO_OBJS[@]} tico/imgui objects"

# ============================================================
# Step 3: Link everything into ELF
# ============================================================
echo "--- Step 3: Linking mupen64plus_tico.elf ---"

ELF_OUTPUT="$BUILD_DIR/mupen64plus_tico.elf"

LINK_FLAGS="-specs=$LIBNX/switch.specs -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
LINK_FLAGS="$LINK_FLAGS -Wl,--gc-sections -Wl,-Map,$BUILD_DIR/mupen64plus_tico.map"

LINK_LIBS="-L$PORTLIBS/lib -L$LIBNX/lib"
LINK_LIBS="$LINK_LIBS -lSDL2_mixer -lmpg123 -lmodplug -lopusfile -lopus -lvorbisidec -logg -lSDL2"
if [ "$USE_VULKAN_FRONTEND" -eq 1 ]; then
    LINK_LIBS="$LINK_LIBS -ldrm_nouveau"
else
    LINK_LIBS="$LINK_LIBS -lEGL -lglapi -ldrm_nouveau"
fi

LINK_LIBS="$LINK_LIBS -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lzstd"
LINK_LIBS="$LINK_LIBS -lnx -lm -lstdc++ -lpthread"

$CXX $LINK_FLAGS \
    "${TICO_OBJS[@]}" \
    "$STATIC_LIB" \
    "${MESA_ARCHIVES[@]}" \
    "${VULKAN_ARCHIVES[@]}" \
    $LINK_LIBS \
    -o "$ELF_OUTPUT"

if [ $? -ne 0 ]; then
    echo "Error: Linking failed"
    exit 1
fi

echo "ELF created: $ELF_OUTPUT"

# ============================================================
# Step 4: Strip and convert to NRO
# ============================================================
echo "--- Step 4: Creating NRO ---"

STRIP="${DEVKITA64}/bin/aarch64-none-elf-strip"
OBJCOPY="${DEVKITA64}/bin/aarch64-none-elf-objcopy"

# Strip debug info (DISABLED for debug build)
# $STRIP --strip-all "$ELF_OUTPUT"

# Remove identifiable sections (DISABLED for debug build)
# $OBJCOPY --remove-section=.comment \
#          --remove-section=.note \
#          --remove-section=.note.gnu.build-id \
#          --remove-section=.note.GNU-stack \
#          "$ELF_OUTPUT"

NRO_OUTPUT="$BUILD_DIR/mupen64plus.nro"
NACPTOOL="$DEVKITPRO/tools/bin/nacptool"
ELF2NRO="$DEVKITPRO/tools/bin/elf2nro"

# Create NACP
NACP_FILE="$BUILD_DIR/mupen64plus_tico.nacp"
$NACPTOOL --create "Mupen64Plus" "tico" "1.0.0" "$NACP_FILE"

# Convert ELF to NRO with romfs
ROMFS_DIR="$BUILD_DIR/romfs"
rm -rf "$ROMFS_DIR"
mkdir -p "$ROMFS_DIR"

[ -d "$TICO_DIR/fonts" ] && cp -r "$TICO_DIR/fonts" "$ROMFS_DIR/"
[ -d "$TICO_DIR/lang" ] && cp -r "$TICO_DIR/lang" "$ROMFS_DIR/"
[ -d "$TICO_DIR/assets" ] && cp -r "$TICO_DIR/assets" "$ROMFS_DIR/"

ELF2NRO_ARGS=(--nacp="$NACP_FILE")

if [ -n "$(find "$ROMFS_DIR" -type f 2>/dev/null | head -1)" ]; then
    echo "Embedding romfs from: $ROMFS_DIR"
    find "$ROMFS_DIR" -type f | while read -r f; do
        echo "  romfs: $f"
    done
    ELF2NRO_ARGS+=(--romfsdir="$ROMFS_DIR")
else
    echo "WARNING: No romfs assets found, NRO will have no embedded romfs"
fi

$ELF2NRO "$ELF_OUTPUT" "$NRO_OUTPUT" "${ELF2NRO_ARGS[@]}"

if [ -f "$NRO_OUTPUT" ]; then
    echo "======================================"
    echo "Build successful!"
    echo "Output: $NRO_OUTPUT"
    echo "Size: $(du -h "$NRO_OUTPUT" | cut -f1)"
    echo "======================================"
else
    echo "Error: mupen64plus_tico.nro not found"
    exit 1
fi
