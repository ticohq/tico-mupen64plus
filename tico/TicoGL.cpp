/// @file TicoGL.cpp
/// @brief OpenGL for GLideN64 and the overlay. See TicoGL.h.

#include "glad.h"

#include "TicoGL.h"
#include "TicoLogger.h"

#include "imgui_impl_opengl3.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <switch.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#define GL_TAG "GL"

namespace TicoGL
{
namespace
{

EGLDisplay s_display = EGL_NO_DISPLAY;
EGLConfig s_config = nullptr;
EGLSurface s_surface = EGL_NO_SURFACE;
// The one context, current on whichever thread holds the context lock: on
// the main thread with the window, on the emulation thread with no surface.
EGLContext s_context = EGL_NO_CONTEXT;
// the emulation thread's surface on drivers without surfaceless contexts
EGLSurface s_corePbuffer = EGL_NO_SURFACE;
bool s_imguiReady = false;

// GLideN64's output
GLuint s_coreFbo = 0;
GLuint s_coreColor = 0;
GLuint s_coreDepth = 0;
uint32_t s_coreWidth = 0;
uint32_t s_coreHeight = 0;

// The frames handed to the main thread
struct Slot
{
    GLuint texture = 0;
    GLuint fbo = 0; // the emulation thread copies into it through this
    uint32_t width = 0;
    uint32_t height = 0;
    GLsync written = nullptr; // the copy into it, from the emulation thread
    GLsync read = nullptr;    // the overlay's last draw of it, from the main thread
};
Slot s_slots[kSlots];
uint32_t s_nextSlot = 0;
std::mutex s_slotMutex;

// Taken in turn (a ticket lock): the emulation thread gives it up once a
// frame and takes it straight back, and must not get it again before the
// main thread waiting for that frame has had its turn.
class TurnLock
{
public:
    void Acquire()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        const uint64_t ticket = m_next++;
        m_turn.wait(lock, [&] { return m_serving == ticket; });
    }
    void Release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_serving++;
        }
        m_turn.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_turn;
    uint64_t m_next = 0;
    uint64_t m_serving = 0;
};

TurnLock s_contextLock;
thread_local int t_lockDepth = 0;
thread_local bool t_coreThread = false;

struct ContextGuard
{
    ContextGuard() { Lock(); }
    ~ContextGuard() { Unlock(); }
};

void LogEglError(const char *what)
{
    LOG_ERROR(GL_TAG, "%s failed: 0x%x", what, eglGetError());
}

// Make the context current on this thread, which holds the lock.
void Attach()
{
    if (!t_coreThread)
    {
        if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context))
            LogEglError("eglMakeCurrent (main thread)");
        return;
    }
    if (s_corePbuffer == EGL_NO_SURFACE &&
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_context))
        return;
    // no EGL_KHR_surfaceless_context: a 1x1 pbuffer it never draws to
    if (s_corePbuffer == EGL_NO_SURFACE)
    {
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        s_corePbuffer = eglCreatePbufferSurface(s_display, s_config, pbufferAttribs);
    }
    if (s_corePbuffer == EGL_NO_SURFACE ||
        !eglMakeCurrent(s_display, s_corePbuffer, s_corePbuffer, s_context))
        LogEglError("eglMakeCurrent (emulation thread)");
}

// Release it, so the other thread can make it current.
void Detach()
{
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

} // namespace

void Lock()
{
    if (t_lockDepth++ == 0)
    {
        s_contextLock.Acquire();
        Attach();
    }
}

void Unlock()
{
    if (--t_lockDepth == 0)
    {
        Detach();
        s_contextLock.Release();
    }
}

int Suspend()
{
    const int depth = t_lockDepth;
    if (depth > 0)
    {
        t_lockDepth = 0;
        Detach();
        s_contextLock.Release();
    }
    return depth;
}

void Resume(int depth)
{
    if (depth > 0)
    {
        s_contextLock.Acquire();
        Attach();
        t_lockDepth = depth;
    }
}

bool Init(uint32_t width, uint32_t height, bool zink)
{
    (void)width;
    (void)height;
    // Mesa's Horizon EGL holds both drivers; the display keeps whichever is
    // selected when it is created.
    setenv("MESA_SWITCH_GL_DRIVER", zink ? "zink" : "nvc0", 1);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", zink ? "zink" : "nouveau", 1);

    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (s_display == EGL_NO_DISPLAY || !eglInitialize(s_display, &major, &minor))
    {
        LogEglError("eglInitialize");
        return false;
    }
    eglBindAPI(EGL_OPENGL_API);

    // Pbuffers are only for the emulation thread on drivers without
    // surfaceless contexts; Mesa 20.1's Horizon EGL has those, and offers
    // window surfaces alone.
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(s_display, configAttribs, &s_config, 1, &count) || count == 0)
    {
        configAttribs[1] = EGL_WINDOW_BIT;
        if (!eglChooseConfig(s_display, configAttribs, &s_config, 1, &count) || count == 0)
        {
            LogEglError("eglChooseConfig");
            return false;
        }
    }

    s_surface = eglCreateWindowSurface(s_display, s_config, (EGLNativeWindowType)nwindowGetDefault(), nullptr);
    if (s_surface == EGL_NO_SURFACE)
    {
        LogEglError("eglCreateWindowSurface");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};
    s_context = eglCreateContext(s_display, s_config, EGL_NO_CONTEXT, contextAttribs);
    if (s_context == EGL_NO_CONTEXT)
    {
        LogEglError("eglCreateContext");
        return false;
    }
    ContextGuard guard;
    if (eglGetCurrentContext() != s_context)
        return false;
    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress))
    {
        LOG_ERROR(GL_TAG, "gladLoadGLLoader failed");
        return false;
    }
    eglSwapInterval(s_display, 1);
    LOG_INFO(GL_TAG, "EGL %d.%d, %s on %s (%s)", major, minor, (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_RENDERER), zink ? "Zink" : "NVC0");

    if (!ImGui_ImplOpenGL3_Init("#version 430 core"))
    {
        LOG_ERROR(GL_TAG, "ImGui_ImplOpenGL3_Init failed");
        return false;
    }
    s_imguiReady = true;

    for (Slot &slot : s_slots)
    {
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

void Shutdown()
{
    if (s_display == EGL_NO_DISPLAY)
        return;
    if (s_context != EGL_NO_CONTEXT)
    {
        ContextGuard guard;
        glFinish();
        if (s_imguiReady)
            ImGui_ImplOpenGL3_Shutdown();
        s_imguiReady = false;
        for (Slot &slot : s_slots)
        {
            if (slot.texture)
                glDeleteTextures(1, &slot.texture);
            if (slot.written)
                glDeleteSync(slot.written);
            if (slot.read)
                glDeleteSync(slot.read);
            slot = Slot();
        }
    }
    if (s_context != EGL_NO_CONTEXT)
        eglDestroyContext(s_display, s_context);
    if (s_corePbuffer != EGL_NO_SURFACE)
        eglDestroySurface(s_display, s_corePbuffer);
    if (s_surface != EGL_NO_SURFACE)
        eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    eglReleaseThread();
    s_display = EGL_NO_DISPLAY;
    s_context = EGL_NO_CONTEXT;
    s_surface = s_corePbuffer = EGL_NO_SURFACE;
}

void *GetProcAddress(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

//==============================================================================
// Emulation thread
//==============================================================================

bool BeginCoreThread()
{
    t_coreThread = true;
    Lock(); // held while emulating, until EndCoreThread
    return eglGetCurrentContext() == s_context;
}

void EndCoreThread()
{
    glFinish();
    for (Slot &slot : s_slots)
    {
        if (slot.fbo)
            glDeleteFramebuffers(1, &slot.fbo);
        slot.fbo = 0;
    }
    if (s_coreFbo)
        glDeleteFramebuffers(1, &s_coreFbo);
    if (s_coreColor)
        glDeleteTextures(1, &s_coreColor);
    if (s_coreDepth)
        glDeleteRenderbuffers(1, &s_coreDepth);
    s_coreFbo = s_coreColor = s_coreDepth = 0;
    s_coreWidth = s_coreHeight = 0;
    Unlock();
    eglReleaseThread();
    t_coreThread = false;
}

unsigned CoreFramebuffer(uint32_t width, uint32_t height)
{
    if (s_coreFbo && width == s_coreWidth && height == s_coreHeight)
        return s_coreFbo;

    GLint previous = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    if (!s_coreFbo)
    {
        glGenFramebuffers(1, &s_coreFbo);
        glGenTextures(1, &s_coreColor);
        glGenRenderbuffers(1, &s_coreDepth);
    }
    glBindTexture(GL_TEXTURE_2D, s_coreColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, s_coreDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, s_coreFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_coreColor, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_coreDepth);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        LOG_ERROR(GL_TAG, "GLideN64 framebuffer incomplete: 0x%x", status);
    glBindFramebuffer(GL_FRAMEBUFFER, previous);
    s_coreWidth = width;
    s_coreHeight = height;
    LOG_INFO(GL_TAG, "GLideN64 renders at %ux%u", width, height);
    return s_coreFbo;
}

uint32_t PostCoreFrame(uint32_t width, uint32_t height)
{
    const uint32_t index = s_nextSlot;
    s_nextSlot = (s_nextSlot + 1) % kSlots;
    Slot &slot = s_slots[index];

    // the overlay's last draw of this slot has to finish first
    GLsync read = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_slotMutex);
        read = slot.read;
        slot.read = nullptr;
    }
    if (read)
    {
        glWaitSync(read, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(read);
    }

    // GLideN64 tracks its own bindings; leave them as they were
    GLint readFbo = 0, drawFbo = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (slot.width != width || slot.height != height)
    {
        GLint texture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glBindTexture(GL_TEXTURE_2D, slot.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, texture);
        slot.width = width;
        slot.height = height;
    }
    if (!slot.fbo)
        glGenFramebuffers(1, &slot.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot.fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, slot.texture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_coreFbo);
    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    if (scissor)
        glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);

    GLsync written = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    GLsync previous = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_slotMutex);
        previous = slot.written;
        slot.written = written;
    }
    if (previous)
        glDeleteSync(previous);
    return index;
}

//==============================================================================
// Main thread
//==============================================================================

namespace
{
// What the frame changes besides ImGui (whose backend restores its own), put
// back for GLideN64, which caches the GL state it sets.
struct SavedState
{
    GLint drawFbo = 0, readFbo = 0;
    GLint viewport[4] = {};
    GLfloat clearColor[4] = {};
    GLboolean colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean scissor = GL_FALSE;
};
SavedState s_saved;
} // namespace

void BeginFrame()
{
    Lock(); // until EndFrame
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s_saved.drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s_saved.readFbo);
    glGetIntegerv(GL_VIEWPORT, s_saved.viewport);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s_saved.clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, s_saved.colorMask);
    s_saved.scissor = glIsEnabled(GL_SCISSOR_TEST);

    if (s_imguiReady)
        ImGui_ImplOpenGL3_NewFrame();
    uint32_t width = 0, height = 0;
    GetSurfaceExtent(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

ImTextureID CoreFrameTexture(uint32_t index, uint32_t &width, uint32_t &height)
{
    if (index >= kSlots)
        return ImTextureID_Invalid;
    Slot &slot = s_slots[index];
    GLsync written = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_slotMutex);
        written = slot.written;
        width = slot.width;
        height = slot.height;
    }
    if (!written || !width || !height)
        return ImTextureID_Invalid;
    glWaitSync(written, 0, GL_TIMEOUT_IGNORED);
    return (ImTextureID)(intptr_t)slot.texture;
}

void EndFrame(ImDrawData *drawData, int index)
{
    if (drawData && s_imguiReady)
        ImGui_ImplOpenGL3_RenderDrawData(drawData);
    if (index >= 0 && index < (int)kSlots)
    {
        GLsync read = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        GLsync previous = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_slotMutex);
            previous = s_slots[index].read;
            s_slots[index].read = read;
        }
        if (previous)
            glDeleteSync(previous);
    }
    eglSwapBuffers(s_display, s_surface);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s_saved.drawFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s_saved.readFbo);
    glViewport(s_saved.viewport[0], s_saved.viewport[1], s_saved.viewport[2], s_saved.viewport[3]);
    glClearColor(s_saved.clearColor[0], s_saved.clearColor[1], s_saved.clearColor[2], s_saved.clearColor[3]);
    glColorMask(s_saved.colorMask[0], s_saved.colorMask[1], s_saved.colorMask[2], s_saved.colorMask[3]);
    if (s_saved.scissor)
        glEnable(GL_SCISSOR_TEST);
    Unlock();
}

bool ReadFrameRGBA(uint32_t index, std::vector<uint8_t> &out, uint32_t &width, uint32_t &height)
{
    ContextGuard guard;
    if (CoreFrameTexture(index, width, height) == ImTextureID_Invalid)
        return false;
    GLuint fbo = 0;
    GLint previous = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_slots[index].texture, 0);
    std::vector<uint8_t> pixels((size_t)width * height * 4);
    GLint packAlignment = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
    glDeleteFramebuffers(1, &fbo);
    // GL rows run bottom-up
    out.resize(pixels.size());
    const size_t row = (size_t)width * 4;
    for (uint32_t y = 0; y < height; y++)
        memcpy(&out[y * row], &pixels[(height - 1 - y) * row], row);
    for (size_t i = 3; i < out.size(); i += 4)
        out[i] = 255;
    return true;
}

void Resize(uint32_t width, uint32_t height)
{
    // the window surface follows the native window's size on the next swap
    (void)width;
    (void)height;
}

void GetSurfaceExtent(uint32_t &width, uint32_t &height)
{
    EGLint w = 0, h = 0;
    eglQuerySurface(s_display, s_surface, EGL_WIDTH, &w);
    eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &h);
    width = (uint32_t)w;
    height = (uint32_t)h;
}

void SetVsync(bool enabled)
{
    ContextGuard guard;
    eglSwapInterval(s_display, enabled ? 1 : 0);
}

ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return ImTextureID_Invalid;
    ContextGuard guard;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint unpackAlignment = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    glBindTexture(GL_TEXTURE_2D, previous);
    return (ImTextureID)(intptr_t)texture;
}

void DestroyTexture(ImTextureID texture)
{
    if (texture == ImTextureID_Invalid)
        return;
    ContextGuard guard;
    GLuint name = (GLuint)(intptr_t)texture;
    glDeleteTextures(1, &name);
}

} // namespace TicoGL
