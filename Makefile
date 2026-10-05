# mupen64plus for the tico frontend, on Horizon (libnx). No libretro: the
# frontend drives the core through its own API (tico/m64p/tico_m64p.h).
# Builds libmupen64plus_tico.a; build_mupen64plus_nro.sh links the NRO.
#
#   make            Release
#   make DEBUG=1    -O0 with debug info
#   make clean

ROOT_DIR := .
TARGET   := libmupen64plus_tico.a
DEBUG    ?= 0

include $(DEVKITPRO)/devkitA64/base_tools
PORTLIBS := $(DEVKITPRO)/portlibs/switch
LIBNX    ?= $(DEVKITPRO)/libnx
STRINGS  := $(PREFIX)strings
AWK      ?= awk
TR       ?= tr

CORE_DIR           := $(ROOT_DIR)/mupen64plus-core
RSPDIR             := $(ROOT_DIR)/mupen64plus-rsp-hle
RSPDIR_PARALLEL    := $(ROOT_DIR)/mupen64plus-rsp-paraLLEl
VIDEODIR_GLIDEN64  := $(ROOT_DIR)/GLideN64
VIDEODIR_PARALLEL  := $(ROOT_DIR)/mupen64plus-video-paraLLEl
TICO_M64P_DIR      := $(ROOT_DIR)/tico/m64p
MINIZIP_DIR        := $(CORE_DIR)/subprojects/minizip
LIBPNG_DIR         := $(ROOT_DIR)/custom/dependencies/libpng
XXHASH_DIR         := $(ROOT_DIR)/xxHash
ZLIB_DIR           := $(ROOT_DIR)/custom/dependencies/libzlib
AWK_DEST_DIR       := $(CORE_DIR)/src/asm_defines
ASM_DEFINES_OBJ    := $(AWK_DEST_DIR)/asm_defines.o

#------------------------------------------------------------------------------
# Sources
#------------------------------------------------------------------------------

CORE_SOURCES_C := \
	$(CORE_DIR)/src/asm_defines/asm_defines.c \
	$(CORE_DIR)/src/api/callbacks.c \
	$(ROOT_DIR)/custom/mupen64plus-core/api/config.c \
	$(CORE_DIR)/src/api/debugger.c \
	$(CORE_DIR)/src/api/frontend.c \
	$(CORE_DIR)/src/backends/plugins_compat/audio_plugin_compat.c \
	$(CORE_DIR)/src/backends/api/video_capture_backend.c \
	$(CORE_DIR)/src/backends/plugins_compat/input_plugin_compat.c \
	$(CORE_DIR)/src/backends/clock_ctime_plus_delta.c \
	$(CORE_DIR)/src/backends/dummy_video_capture.c \
	$(CORE_DIR)/src/backends/file_storage.c \
	$(CORE_DIR)/src/device/cart/cart.c \
	$(CORE_DIR)/src/device/cart/af_rtc.c \
	$(CORE_DIR)/src/device/cart/cart_rom.c \
	$(CORE_DIR)/src/device/cart/eeprom.c \
	$(CORE_DIR)/src/device/cart/flashram.c \
	$(CORE_DIR)/src/device/cart/is_viewer.c \
	$(CORE_DIR)/src/device/cart/sram.c \
	$(CORE_DIR)/src/device/controllers/game_controller.c \
	$(CORE_DIR)/src/device/controllers/vru_controller.c \
	$(CORE_DIR)/src/device/controllers/paks/biopak.c \
	$(CORE_DIR)/src/device/controllers/paks/mempak.c \
	$(CORE_DIR)/src/device/controllers/paks/rumblepak.c \
	$(CORE_DIR)/src/device/controllers/paks/transferpak.c \
	$(CORE_DIR)/src/device/dd/dd_controller.c \
	$(CORE_DIR)/src/device/dd/disk.c \
	$(CORE_DIR)/src/device/device.c \
	$(CORE_DIR)/src/device/gb/gb_cart.c \
	$(CORE_DIR)/src/device/gb/mbc3_rtc.c \
	$(CORE_DIR)/src/device/gb/m64282fp.c \
	$(CORE_DIR)/src/device/memory/memory.c \
	$(CORE_DIR)/src/device/pif/bootrom_hle.c \
	$(CORE_DIR)/src/device/pif/cic.c \
	$(CORE_DIR)/src/device/pif/n64_cic_nus_6105.c \
	$(CORE_DIR)/src/device/pif/pif.c \
	$(CORE_DIR)/src/device/r4300/cached_interp.c \
	$(CORE_DIR)/src/device/r4300/cp0.c \
	$(CORE_DIR)/src/device/r4300/cp1.c \
	$(CORE_DIR)/src/device/r4300/cp2.c \
	$(CORE_DIR)/src/device/r4300/idec.c \
	$(CORE_DIR)/src/device/r4300/interrupt.c \
	$(CORE_DIR)/src/device/r4300/pure_interp.c \
	$(CORE_DIR)/src/device/r4300/r4300_core.c \
	$(CORE_DIR)/src/device/r4300/tlb.c \
	$(CORE_DIR)/src/device/rcp/ai/ai_controller.c \
	$(CORE_DIR)/src/device/rcp/mi/mi_controller.c \
	$(CORE_DIR)/src/device/rcp/pi/pi_controller.c \
	$(CORE_DIR)/src/device/rcp/rdp/fb.c \
	$(CORE_DIR)/src/device/rcp/rdp/rdp_core.c \
	$(CORE_DIR)/src/device/rcp/ri/ri_controller.c \
	$(CORE_DIR)/src/device/rcp/rsp/rsp_core.c \
	$(CORE_DIR)/src/device/rcp/si/si_controller.c \
	$(CORE_DIR)/src/device/rcp/vi/vi_controller.c \
	$(CORE_DIR)/src/device/rdram/rdram.c \
	$(CORE_DIR)/src/main/main.c \
	$(CORE_DIR)/src/main/util.c \
	$(CORE_DIR)/src/main/cheat.c \
	$(CORE_DIR)/src/main/rom.c \
	$(CORE_DIR)/src/main/savestates.c \
	$(CORE_DIR)/src/plugin/plugin.c \
	$(CORE_DIR)/src/plugin/dummy_audio.c \
	$(CORE_DIR)/src/plugin/dummy_input.c

# the tico frontend's side of the core: options, emulation thread, states,
# audio, input and the video extension
TICO_M64P_SOURCES_C := \
	$(TICO_M64P_DIR)/tico_m64p.c \
	$(TICO_M64P_DIR)/audio_tico.c \
	$(TICO_M64P_DIR)/input_tico.c \
	$(TICO_M64P_DIR)/vidext_tico.c

MINIZIP_SOURCES_C = \
	$(MINIZIP_DIR)/zip.c \
	$(MINIZIP_DIR)/unzip.c \
	$(MINIZIP_DIR)/ioapi.c

LIBPNG_SOURCES_C = \
	$(LIBPNG_DIR)/png.c \
	$(LIBPNG_DIR)/pngerror.c \
	$(LIBPNG_DIR)/pngget.c \
	$(LIBPNG_DIR)/pngmem.c \
	$(LIBPNG_DIR)/pngpread.c \
	$(LIBPNG_DIR)/pngread.c \
	$(LIBPNG_DIR)/pngrio.c \
	$(LIBPNG_DIR)/pngrtran.c \
	$(LIBPNG_DIR)/pngrutil.c \
	$(LIBPNG_DIR)/pngset.c \
	$(LIBPNG_DIR)/pngtrans.c \
	$(LIBPNG_DIR)/pngwio.c \
	$(LIBPNG_DIR)/pngwrite.c \
	$(LIBPNG_DIR)/pngwtran.c \
	$(LIBPNG_DIR)/pngwutil.c

ZLIB_SOURCES_C = \
	$(ZLIB_DIR)/adler32.c \
	$(ZLIB_DIR)/compress.c \
	$(ZLIB_DIR)/crc32.c \
	$(ZLIB_DIR)/deflate.c \
	$(ZLIB_DIR)/gzclose.c \
	$(ZLIB_DIR)/gzlib.c \
	$(ZLIB_DIR)/gzread.c \
	$(ZLIB_DIR)/gzwrite.c \
	$(ZLIB_DIR)/infback.c \
	$(ZLIB_DIR)/inffast.c \
	$(ZLIB_DIR)/inflate.c \
	$(ZLIB_DIR)/inftrees.c \
	$(ZLIB_DIR)/trees.c \
	$(ZLIB_DIR)/uncompr.c \
	$(ZLIB_DIR)/zutil.c

RSP_HLE_SOURCES_C := \
	$(RSPDIR)/src/alist.c \
	$(RSPDIR)/src/alist_audio.c \
	$(RSPDIR)/src/alist_naudio.c \
	$(RSPDIR)/src/alist_nead.c \
	$(RSPDIR)/src/audio.c \
	$(RSPDIR)/src/cicx105.c \
	$(RSPDIR)/src/hle.c \
	$(RSPDIR)/src/hvqm.c \
	$(RSPDIR)/src/jpeg.c \
	$(RSPDIR)/src/memory.c \
	$(RSPDIR)/src/mp3.c \
	$(RSPDIR)/src/musyx.c \
	$(RSPDIR)/src/re2.c \
	$(RSPDIR)/src/plugin.c


GLIDEN64_SOURCES_CXX := \
	$(VIDEODIR_GLIDEN64)/src/Combiner.cpp                                                         \
    $(VIDEODIR_GLIDEN64)/src/CombinerKey.cpp                                                      \
    $(VIDEODIR_GLIDEN64)/src/CommonPluginAPI.cpp                                                  \
    $(VIDEODIR_GLIDEN64)/src/Config.cpp                                                           \
    $(VIDEODIR_GLIDEN64)/src/convert.cpp                                                          \
    $(VIDEODIR_GLIDEN64)/src/DebugDump.cpp                                                        \
    $(VIDEODIR_GLIDEN64)/src/Debugger.cpp                                                         \
    $(VIDEODIR_GLIDEN64)/src/DepthBuffer.cpp                                                      \
    $(VIDEODIR_GLIDEN64)/src/DisplayWindow.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/mupen64plus/mupen64plus_DisplayWindow.cpp     \
    $(VIDEODIR_GLIDEN64)/src/DisplayLoadProgress.cpp                                              \
    $(VIDEODIR_GLIDEN64)/src/FrameBuffer.cpp                                                      \
    $(VIDEODIR_GLIDEN64)/src/FrameBufferInfo.cpp                                                  \
    $(VIDEODIR_GLIDEN64)/src/GBI.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/gDP.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/GLideN64.cpp                                                         \
    $(VIDEODIR_GLIDEN64)/src/gSP.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/N64.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/TextDrawer.cpp                                                       \
    $(VIDEODIR_GLIDEN64)/src/PaletteTexture.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/Performance.cpp                                                      \
    $(VIDEODIR_GLIDEN64)/src/PostProcessor.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/RDP.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/RSP.cpp                                                              \
    $(VIDEODIR_GLIDEN64)/src/SoftwareRender.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/TexrectDrawer.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/TextureFilterHandler.cpp                                             \
    $(VIDEODIR_GLIDEN64)/src/Textures.cpp                                                         \
    $(VIDEODIR_GLIDEN64)/src/VI.cpp                                                               \
    $(VIDEODIR_GLIDEN64)/src/ZlutTexture.cpp                                                      \
    $(VIDEODIR_GLIDEN64)/src/common/CommonAPIImpl_common.cpp                                      \
    $(VIDEODIR_GLIDEN64)/src/DepthBufferRender/ClipPolygon.cpp                                    \
    $(VIDEODIR_GLIDEN64)/src/DepthBufferRender/DepthBufferRender.cpp                              \
    $(VIDEODIR_GLIDEN64)/src/BufferCopy/BlueNoiseTexture.cpp                                    \
    $(VIDEODIR_GLIDEN64)/src/BufferCopy/ColorBufferToRDRAM.cpp                                    \
    $(VIDEODIR_GLIDEN64)/src/BufferCopy/DepthBufferToRDRAM.cpp                                    \
    $(VIDEODIR_GLIDEN64)/src/BufferCopy/RDRAMtoColorBuffer.cpp                                    \
    $(VIDEODIR_GLIDEN64)/src/GraphicsDrawer.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/Context.cpp                                                 \
    $(VIDEODIR_GLIDEN64)/src/Graphics/ColorBufferReader.cpp                                       \
    $(VIDEODIR_GLIDEN64)/src/Graphics/CombinerProgram.cpp                                         \
    $(VIDEODIR_GLIDEN64)/src/Graphics/ObjectHandle.cpp                                            \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLFunctions.cpp                               \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/ThreadedOpenGl/opengl_Wrapper.cpp             \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/ThreadedOpenGl/opengl_WrappedFunctions.cpp    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/ThreadedOpenGl/opengl_Command.cpp             \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/ThreadedOpenGl/opengl_ObjectPool.cpp          \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/ThreadedOpenGl/RingBufferPool.cpp             \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_Attributes.cpp                         \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_BufferedDrawer.cpp                     \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_BufferManipulationObjectFactory.cpp    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_CachedFunctions.cpp                    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_ColorBufferReaderWithBufferStorage.cpp \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_ColorBufferReaderWithPixelBuffer.cpp   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_ColorBufferReaderWithReadPixels.cpp    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_ColorBufferReaderWithEGLImage.cpp      \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_ContextImpl.cpp                        \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_GLInfo.cpp                             \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_Parameters.cpp                         \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_TextureManipulationObjectFactory.cpp   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_UnbufferedDrawer.cpp                   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/opengl_Utils.cpp                              \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerInputs.cpp                  \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilder.cpp          \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramImpl.cpp             \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactory.cpp   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryAccurate.cpp \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryFast.cpp     \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramUniformFactoryCommon.cpp   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderCommon.cpp    \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderAccurate.cpp  \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderFast.cpp      \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_FXAA.cpp                            \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_ShaderStorage.cpp                   \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_SpecialShadersFactory.cpp           \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GLSL/glsl_Utils.cpp                           \
    $(VIDEODIR_GLIDEN64)/src/Graphics/OpenGLContext/GraphicBuffer/PrivateApi/GraphicBuffer.cpp    \
    $(VIDEODIR_GLIDEN64)/src/mupenplus/MemoryStatus_mupenplus.cpp                                 \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3D.cpp                                                       \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DAM.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DBETA.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DDKR.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX2.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX3.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX095.cpp                                                  \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX2ACCLAIM.cpp                                             \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DEX2CBFD.cpp                                                \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DZEX2.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DFLX2.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DGOLDEN.cpp                                                 \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DPD.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DSETA.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F5Indi_Naboo.cpp                                              \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F5Rogue.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/F3DTEXA.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/L3D.cpp                                                       \
    $(VIDEODIR_GLIDEN64)/src/uCodes/L3DEX2.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/uCodes/L3DEX.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/S2DEX2.cpp                                                    \
    $(VIDEODIR_GLIDEN64)/src/uCodes/S2DEX.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/T3DUX.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/Turbo3D.cpp                                                   \
    $(VIDEODIR_GLIDEN64)/src/uCodes/ZSort.cpp                                                     \
    $(VIDEODIR_GLIDEN64)/src/uCodes/ZSortBOSS.cpp                                                 \
	$(VIDEODIR_GLIDEN64)/src/MupenPlusPluginAPI.cpp                                               \
	$(VIDEODIR_GLIDEN64)/src/mupenplus/MupenPlusAPIImpl.cpp                                       \
	$(ROOT_DIR)/custom/GLideN64/mupenplus/Config_mupenplus.cpp                                    \
	$(ROOT_DIR)/custom/GLideN64/mupenplus/CommonAPIImpl_mupenplus.cpp							  \
	$(VIDEODIR_GLIDEN64)/src/Log.cpp

GLIDEN64_SOURCES_CXX += \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TextureFilters.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TextureFilters_2xsai.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TextureFilters_hq2x.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TextureFilters_hq4x.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TextureFilters_xbrz.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxCache.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxDbg.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxFilter.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxFilterExport.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxHiResCache.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxHiResNoCache.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxHiResLoader.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxImage.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxQuantize.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxReSample.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxTexCache.cpp \
	$(VIDEODIR_GLIDEN64)/src/GLideNHQ/TxUtil.cpp \
	$(VIDEODIR_GLIDEN64)/src/RSP_LoadMatrix.cpp

GLIDEN64_SOURCES_CXX += $(VIDEODIR_GLIDEN64)/src/CRC32_ARMV8.cpp $(VIDEODIR_GLIDEN64)/src/3DMath.cpp
GLIDEN64_SOURCES_C := $(VIDEODIR_GLIDEN64)/src/osal/osal_files_unix.c

PARALLEL_RDP_IMPLEMENTATION := $(VIDEODIR_PARALLEL)/parallel-rdp
include $(PARALLEL_RDP_IMPLEMENTATION)/config.mk

PARALLEL_RSP_SOURCES_CXX := \
	$(RSPDIR_PARALLEL)/parallel.cpp \
	$(RSPDIR_PARALLEL)/rsp_disasm.cpp \
	$(RSPDIR_PARALLEL)/jit_allocator.cpp \
	$(RSPDIR_PARALLEL)/rsp_jit.cpp \
	$(wildcard $(RSPDIR_PARALLEL)/rsp/*.cpp) \
	$(wildcard $(RSPDIR_PARALLEL)/arch/simd/rsp/*.cpp)
PARALLEL_RSP_SOURCES_C := \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_disasm.c \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_memory.c \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_names.c \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_note.c \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_print.c \
	$(RSPDIR_PARALLEL)/lightning/lib/jit_size.c \
	$(RSPDIR_PARALLEL)/lightning/lib/lightning.c

DYNAREC_SOURCES_C   := $(CORE_DIR)/src/device/r4300/new_dynarec/new_dynarec.c
DYNAREC_SOURCES_ASM := $(CORE_DIR)/src/device/r4300/new_dynarec/arm64/linkage_arm64.S

SOURCES_C := $(CORE_SOURCES_C) $(TICO_M64P_SOURCES_C) $(CORE_DIR)/subprojects/md5/md5.c \
	$(MINIZIP_SOURCES_C) $(LIBPNG_SOURCES_C) $(ZLIB_SOURCES_C) $(RSP_HLE_SOURCES_C) \
	$(GLIDEN64_SOURCES_C) $(PARALLEL_RDP_SOURCES_C) $(PARALLEL_RSP_SOURCES_C) $(DYNAREC_SOURCES_C)
SOURCES_CXX := $(GLIDEN64_SOURCES_CXX) $(PARALLEL_RDP_SOURCES_CXX) \
	$(VIDEODIR_PARALLEL)/parallel.cpp $(VIDEODIR_PARALLEL)/rdp.cpp $(PARALLEL_RSP_SOURCES_CXX)
SOURCES_ASM := $(DYNAREC_SOURCES_ASM)

OBJECTS := $(SOURCES_CXX:.cpp=.o) $(SOURCES_C:.c=.o) $(SOURCES_ASM:.S=.o)

#------------------------------------------------------------------------------
# Flags
#------------------------------------------------------------------------------

INCFLAGS := \
	-I$(ROOT_DIR)/custom \
	-I$(ROOT_DIR)/custom/mupen64plus-core \
	-I$(ROOT_DIR)/custom/GLideN64 \
	-I$(VIDEODIR_GLIDEN64)/src \
	-I$(VIDEODIR_GLIDEN64)/src/osal \
	-I$(VIDEODIR_GLIDEN64)/src/inc \
	-I$(CORE_DIR)/src \
	-I$(CORE_DIR)/src/api \
	-I$(CORE_DIR)/subprojects/md5 \
	-I$(MINIZIP_DIR) -I$(LIBPNG_DIR) -I$(XXHASH_DIR) -I$(ZLIB_DIR) \
	-I$(RSPDIR_PARALLEL)/arch/simd/rsp -I$(RSPDIR_PARALLEL)/lightning/include \
	-I$(ROOT_DIR)/switch \
	$(PARALLEL_RDP_INCLUDE_DIRS) \
	-I$(PORTLIBS)/include -I$(LIBNX)/include

DEFINES := -D__SWITCH__=1 -DSWITCH -DHAVE_LIBNX -DOS_LINUX -DEGL -DVK_USE_PLATFORM_VI_NN \
	-DTICO_M64P -DM64P_PLUGIN_API -DM64P_CORE_PROTOTYPES -DMUPENPLUSAPI \
	-D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -DUSE_FILE32API -D_ENDUSER_RELEASE \
	-DTXFILTER_LIB -D__VEC4_OPT -D_GLIBCXX_USE_C99_MATH_TR1 -D_LDBL_EQ_DBL -DCORE \
	-DHAVE_OPENGL -DHAVE_PARALLEL_RDP -DHAVE_PARALLEL_RSP -DPARALLEL_INTEGRATION \
	-DNEW_DYNAREC=4 -DDYNAREC -I$(AWK_DEST_DIR)/

ARCH := -march=armv8-a+crc -mtune=cortex-a57 -mtp=soft -mcpu=cortex-a57+crc+fp+simd -fPIE \
	-specs=$(LIBNX)/switch.specs
ifeq ($(DEBUG), 1)
OPT := -O0 -g
else
OPT := -O3 -g -DNDEBUG -fsigned-char -ffast-math -funsafe-math-optimizations -fno-strict-aliasing \
	-fomit-frame-pointer -funroll-loops
endif

COMMON := $(ARCH) $(OPT) $(DEFINES) $(INCFLAGS) -ffunction-sections -fdata-sections \
	-ftls-model=local-exec -fcommon -fPIC -MMD -MP
CFLAGS   := $(COMMON) -std=gnu11 -Wno-discarded-qualifiers $(PARALLEL_RDP_CFLAGS)
CXXFLAGS := $(COMMON) -std=gnu++14 -fno-rtti -fvisibility-inlines-hidden $(PARALLEL_RDP_CXXFLAGS)

#------------------------------------------------------------------------------
# Rules
#------------------------------------------------------------------------------

.DEFAULT_GOAL := all
-include $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

# The dynarec's assembly reads struct offsets generated from the core.
$(AWK_DEST_DIR)/asm_defines_gas.h: $(AWK_DEST_DIR)/asm_defines_nasm.h
$(AWK_DEST_DIR)/asm_defines_nasm.h: $(ASM_DEFINES_OBJ)
	$(STRINGS) "$<" | $(TR) -d '\r' | $(AWK) -v dest_dir="$(AWK_DEST_DIR)" -f $(CORE_DIR)/tools/gen_asm_defines.awk

%.o: %.S $(AWK_DEST_DIR)/asm_defines_gas.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(RSPDIR_PARALLEL)/lightning/lib/lightning.o: $(RSPDIR_PARALLEL)/lightning/lib/lightning.c
	$(CC) $(CFLAGS) -DHAVE_MMAP=1 -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	find $(ROOT_DIR) -path $(ROOT_DIR)/tico -prune -o \( -name "*.o" -o -name "*.d" \) -type f -print -delete >/dev/null
	rm -f $(TARGET) $(AWK_DEST_DIR)/asm_defines_gas.h $(AWK_DEST_DIR)/asm_defines_nasm.h

.PHONY: all clean
