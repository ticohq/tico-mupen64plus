/// @file TicoAudio.h
/// @brief Reusable audio manager for Tico libretro frontend
/// Uses callback-based audio with ring buffer and resampling

#pragma once

#include <SDL.h>
#include <SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
extern "C" {
enum rsp_plugin_type
{
    RSP_PLUGIN_NONE = 0,
    RSP_PLUGIN_HLE,
    RSP_PLUGIN_CXD4,
    RSP_PLUGIN_PARALLEL,
    RSP_PLUGIN_MAX
};

extern enum rsp_plugin_type current_rsp_type;
}
#endif
#include "TicoLogger.h"

/// @brief Thread-safe lock-free SPSC ring buffer for audio samples
template <typename T>
class TicoRingBuffer
{
public:
    explicit TicoRingBuffer(size_t size) : m_buffer(size), m_head(0), m_tail(0) {}

    void Write(const T *data, size_t count)
    {
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t freeSpace = (capacity + currentHead - currentTail - 1) % capacity;
        size_t toWrite = (count < freeSpace) ? count : freeSpace;

        if (toWrite == 0)
            return;

        size_t part1 = std::min(toWrite, capacity - currentTail);
        std::copy(data, data + part1, m_buffer.begin() + currentTail);

        if (part1 < toWrite)
        {
            std::copy(data + part1, data + toWrite, m_buffer.begin());
        }

        m_tail.store((currentTail + toWrite) % capacity, std::memory_order_release);
    }

    size_t Read(T *data, size_t count)
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t available = (capacity + currentTail - currentHead) % capacity;
        size_t toRead = (count < available) ? count : available;

        if (toRead == 0)
            return 0;

        size_t part1 = std::min(toRead, capacity - currentHead);
        std::copy(m_buffer.begin() + currentHead,
                  m_buffer.begin() + currentHead + part1, data);

        if (part1 < toRead)
        {
            size_t part2 = toRead - part1;
            std::copy(m_buffer.begin(), m_buffer.begin() + part2, data + part1);
        }

        m_head.store((currentHead + toRead) % capacity, std::memory_order_release);
        return toRead;
    }

    size_t Available() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        return (capacity + currentTail - currentHead) % capacity;
    }

    size_t GetAvailableWrite() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        size_t used = (capacity + currentTail - currentHead) % capacity;
        return (capacity - 1) - used;
    }

    void Clear()
    {
        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
    }

private:
    std::vector<T> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};

/// @brief Audio manager for libretro cores
/// @details Uses Mix_HookMusic callback with ring buffer and optional resampling
class TicoAudio
{
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int CHANNELS = 2;
    static constexpr size_t BUFFER_SIZE = SAMPLE_RATE * 6;
    
    // Strict 50ms latency target to prevent cumulative audio delay
    static constexpr size_t MAX_BUFFERED_SAMPLES = (SAMPLE_RATE * 50 / 1000) * CHANNELS; // 50ms (4410 samples)
    static constexpr size_t SDL_QUEUE_MAX_BYTES = MAX_BUFFERED_SAMPLES * sizeof(int16_t);

    TicoAudio() : m_buffer(BUFFER_SIZE), m_resampler(nullptr), m_deviceId(0),
                  m_initialized(false), m_paused(false), m_coreSampleRate(SAMPLE_RATE) {}

    ~TicoAudio()
    {
        Shutdown();
    }

    /// Initialize audio system
    bool Init(SDL_AudioDeviceID deviceId = 0)
    {
        if (m_initialized)
            return true;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (deviceId == 0)
            {
                LOG_ERROR("AUDIO", "SDL_QueueAudio mode requires a valid device ID");
                return false;
            }
            m_deviceId = deviceId;
            SDL_PauseAudioDevice(m_deviceId, 0);
            LOG_AUDIO("Initialized with SDL_QueueAudio (Push)");
        }
        else
        {
            Mix_HookMusic(AudioCallback, this);
            LOG_AUDIO("Initialized with callback-based audio");
        }

        m_initialized = true;
        return true;
    }

    /// Shutdown audio system
    void Shutdown()
    {
        if (!m_initialized)
            return;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_PauseAudioDevice(m_deviceId, 1);
            SDL_CloseAudioDevice(m_deviceId);
            m_deviceId = 0;
        }
        else
        {
            Mix_HookMusic(nullptr, nullptr);
        }

        if (m_resampler)
        {
            SDL_FreeAudioStream(m_resampler);
            m_resampler = nullptr;
        }

        m_initialized = false;
        LOG_AUDIO("Shutdown complete");
    }

    /// Set the core's sample rate for resampling
    void SetCoreSampleRate(double sampleRate)
    {
        if (sampleRate <= 0)
            sampleRate = SAMPLE_RATE;

        int coreSR = static_cast<int>(sampleRate);
        if (coreSR == m_coreSampleRate && m_resampler != nullptr)
        {
            return;
        }

        m_coreSampleRate = coreSR;

        if (m_resampler)
        {
            SDL_FreeAudioStream(m_resampler);
            m_resampler = nullptr;
        }

        if (coreSR != SAMPLE_RATE)
        {
            m_resampler = SDL_NewAudioStream(
                AUDIO_S16, CHANNELS, coreSR,
                AUDIO_S16, CHANNELS, SAMPLE_RATE);
            if (m_resampler)
            {
                LOG_AUDIO("Resampler created: %d -> %d Hz", coreSR, SAMPLE_RATE);
            }
        }
    }

    /// Push a single audio sample (left, right)
    void PushSample(int16_t left, int16_t right)
    {
        if (!m_initialized)
            return;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            while (SDL_GetQueuedAudioSize(m_deviceId) >= SDL_QUEUE_MAX_BYTES && !m_paused)
            {
#ifdef __SWITCH__
                svcSleepThread(100000); // 100µs yield
#else
                SDL_Delay(1);
#endif
            }

            int16_t samples[2] = {left, right};
            SDL_QueueAudio(m_deviceId, samples, sizeof(samples));
        }
        else
        {
            while (m_buffer.Available() >= MAX_BUFFERED_SAMPLES && !m_paused)
            {
#ifdef __SWITCH__
                svcSleepThread(100000);
#else
                SDL_Delay(1);
#endif
            }

            int16_t samples[2] = {left, right};
            m_buffer.Write(samples, 2);
        }
    }

    /// Push a batch of audio samples
    size_t PushSamples(const int16_t *data, size_t frames)
    {
        if (!m_initialized || !data || frames == 0)
            return 0;

        size_t samplesNeeded = frames * CHANNELS;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            while (SDL_GetQueuedAudioSize(m_deviceId) >= SDL_QUEUE_MAX_BYTES && !m_paused)
            {
#ifdef __SWITCH__
                svcSleepThread(100000);
#else
                SDL_Delay(1);
#endif
            }

            SDL_QueueAudio(m_deviceId, data, samplesNeeded * sizeof(int16_t));
        }
        else
        {
            while (m_buffer.Available() >= MAX_BUFFERED_SAMPLES && !m_paused)
            {
#ifdef __SWITCH__
                svcSleepThread(100000);
#else
                SDL_Delay(1);
#endif
            }

            m_buffer.Write(data, samplesNeeded);
        }
        return frames;
    }

    /// Flush/clear the audio buffer
    void Flush()
    {
        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_ClearQueuedAudio(m_deviceId);
        }
        else
        {
            m_buffer.Clear();
            if (m_resampler)
            {
                SDL_AudioStreamClear(m_resampler);
            }
        }
        LOG_AUDIO("Buffer flushed");
    }

    /// Pause/unpause
    void SetPaused(bool paused) { m_paused = paused; }
    bool IsPaused() const { return m_paused; }

private:
    /// @brief SDL_mixer pull callback — reads from ring buffer with optional resampling
    static void AudioCallback(void *userdata, uint8_t *stream, int len)
    {
        TicoAudio *self = static_cast<TicoAudio *>(userdata);
        if (!self)
        {
            memset(stream, 0, len);
            return;
        }

#ifdef __SWITCH__
        static thread_local bool s_audioThreadPinned = false;
        if (!s_audioThreadPinned)
        {
            const int preferredCore = (current_rsp_type == RSP_PLUGIN_PARALLEL) ? 0 : 1;
            Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferredCore, 1u << preferredCore);
            if (R_SUCCEEDED(rc))
                LOG_AUDIO("Pinned audio callback thread to core %d", preferredCore);
            s_audioThreadPinned = true;
        }
#endif

        memset(stream, 0, len);

        size_t bytesRead = 0;
        const size_t requestedSamples = static_cast<size_t>(len) / sizeof(int16_t);

        if (self->m_resampler)
        {
            int availableBytes = SDL_AudioStreamAvailable(self->m_resampler);

            while (availableBytes < len)
            {
                int16_t tempBuf[4096];
                size_t bufferedSamples = self->m_buffer.Available();
                size_t toRead = std::min(bufferedSamples, sizeof(tempBuf) / sizeof(tempBuf[0]));
                if (toRead == 0)
                    break;

                size_t read = self->m_buffer.Read(tempBuf, toRead);
                if (read == 0)
                    break;

                SDL_AudioStreamPut(self->m_resampler, tempBuf, read * sizeof(int16_t));
                availableBytes = SDL_AudioStreamAvailable(self->m_resampler);
            }

            int resampled = SDL_AudioStreamGet(self->m_resampler, stream, len);
            if (resampled > 0)
                bytesRead = static_cast<size_t>(resampled);
        }
        else
        {
            size_t bufferedSamples = self->m_buffer.Available();
            size_t toRead = std::min(bufferedSamples, requestedSamples);
            size_t read = self->m_buffer.Read(reinterpret_cast<int16_t *>(stream), toRead);
            bytesRead = read * sizeof(int16_t);
        }

        if (bytesRead < static_cast<size_t>(len) && !self->m_paused)
        {
            uint32_t underrunCount = self->m_underrunCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((underrunCount % 120) == 1)
            {
                LOG_WARN("AUDIO", "Audio underrun #%u (requested=%d bytes, provided=%zu bytes, buffered=%zu samples)",
                         underrunCount, len, bytesRead, self->m_buffer.Available());
            }
        }
    }

    TicoRingBuffer<int16_t> m_buffer;
    SDL_AudioStream *m_resampler;
    SDL_AudioDeviceID m_deviceId;
    bool m_initialized;
    bool m_paused;
    int m_coreSampleRate;
    std::atomic<uint32_t> m_underrunCount{0};
};
