#include "openal_ring.h"

#include "../../platform/os.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <mutex>

#if defined(__APPLE__)
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#endif

namespace {

bool enabled() {
    static int on = -1;
    if (on < 0) {
        const char *v = recomp_env("AUDIO_OPENAL");
        on = 1;
        if (v && v[0] && (strcmp(v, "0") == 0 || strcmp(v, "off") == 0 || strcmp(v, "false") == 0 ||
                          strcmp(v, "no") == 0))
            on = 0;
        fprintf(stderr,
                "[host] audio experiment: OpenAL Soft DirectSound playback is %s. "
                "RECOMP_AUDIO_OPENAL=0 restores the SDL mixer.\n",
                on ? "ON" : "OFF");
        fflush(stderr);
    }
    return on != 0;
}

#if defined(__APPLE__)

using BufferStaticFn = void(AL_APIENTRY *)(ALuint, ALenum, ALvoid *, ALsizei, ALsizei);

struct Voice {
    bool live = false;
    ALuint source = 0;
    ALuint buffer = 0;
    uint32_t bytes = 0;
};

std::mutex g_mu;
ALCdevice *g_device = nullptr;
ALCcontext *g_ctx = nullptr;
BufferStaticFn g_buffer_static = nullptr;
bool g_ready = false;
bool g_failed = false;
int g_refresh = 50;
std::map<uint32_t, Voice> g_voices;

float gain_from_mb(int32_t mb) {
    if (mb <= -10000)
        return 0.0f;
    if (mb >= 0)
        return 1.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

void bind_context() {
    if (g_ctx && alcGetCurrentContext() != g_ctx)
        alcMakeContextCurrent(g_ctx);
}

bool prepare_locked() {
    if (g_ready)
        return true;
    if (g_failed)
        return false;
    g_device = alcOpenDevice(nullptr);
    if (!g_device) {
        fprintf(stderr, "[host] audio experiment: OpenAL Soft device did not open\n");
        fflush(stderr);
        g_failed = true;
        return false;
    }
    ALCint attr[] = {ALC_FREQUENCY, 48000, 0};
    g_ctx = alcCreateContext(g_device, attr);
    if (!g_ctx) {
        fprintf(stderr, "[host] audio experiment: OpenAL Soft context did not open\n");
        fflush(stderr);
        alcCloseDevice(g_device);
        g_device = nullptr;
        g_failed = true;
        return false;
    }
    alcMakeContextCurrent(g_ctx);
    if (!alIsExtensionPresent("AL_EXT_STATIC_BUFFER")) {
        fprintf(stderr,
                "[host] audio experiment: OpenAL Soft has no AL_EXT_STATIC_BUFFER; "
                "the mix ring stays on SDL\n");
        fflush(stderr);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(g_ctx);
        alcCloseDevice(g_device);
        g_ctx = nullptr;
        g_device = nullptr;
        g_failed = true;
        return false;
    }
    g_buffer_static = (BufferStaticFn)alGetProcAddress("alBufferDataStatic");
    if (!g_buffer_static) {
        fprintf(stderr, "[host] audio experiment: alBufferDataStatic is missing\n");
        fflush(stderr);
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(g_ctx);
        alcCloseDevice(g_device);
        g_ctx = nullptr;
        g_device = nullptr;
        g_failed = true;
        return false;
    }
    ALCint refresh = 0;
    alcGetIntegerv(g_device, ALC_REFRESH, 1, &refresh);
    if (refresh > 0)
        g_refresh = refresh;
    g_ready = true;
    fprintf(stderr, "[host] audio experiment: OpenAL Soft device is open at 48000 Hz, refresh %d\n",
            g_refresh);
    fflush(stderr);
    return true;
}

void release_voice_locked(Voice &v) {
    if (!v.live)
        return;
    bind_context();
    if (v.source) {
        alSourceStop(v.source);
        alSourcei(v.source, AL_BUFFER, 0);
        alDeleteSources(1, &v.source);
    }
    if (v.buffer)
        alDeleteBuffers(1, &v.buffer);
    v = Voice{};
    alGetError();
}

uint32_t position_locked(const Voice &v) {
    if (!v.live || !v.bytes)
        return 0;
    bind_context();
    ALint ofs = 0;
    alGetSourcei(v.source, AL_BYTE_OFFSET, &ofs);
    if (ofs < 0)
        return 0;
    return (uint32_t)ofs % v.bytes;
}

#endif

} // namespace

int host_openal_ring_enabled(void) {
    return enabled() ? 1 : 0;
}

int host_openal_ring_play(uint32_t id, const void *pcm, uint32_t bytes, uint32_t start_offset,
                          int32_t volume_mb) {
#if !defined(__APPLE__)
    (void)id;
    (void)pcm;
    (void)bytes;
    (void)start_offset;
    (void)volume_mb;
    return 0;
#else
    if (!enabled() || !pcm || bytes != 384000)
        return 0;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!prepare_locked())
        return 0;
    bind_context();
    Voice &v = g_voices[id];
    release_voice_locked(v);

    ALuint buffer = 0, source = 0;
    alGenBuffers(1, &buffer);
    g_buffer_static(buffer, AL_FORMAT_STEREO16, (ALvoid *)pcm, (ALsizei)bytes, 48000);
    if (alGetError() != AL_NO_ERROR || !buffer) {
        if (buffer)
            alDeleteBuffers(1, &buffer);
        fprintf(stderr, "[host] audio experiment: OpenAL static buffer was refused\n");
        fflush(stderr);
        return 0;
    }
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, (ALint)buffer);
    alSourcei(source, AL_LOOPING, AL_TRUE);
    alSourcef(source, AL_GAIN, gain_from_mb(volume_mb));
    if (start_offset && start_offset < bytes)
        alSourcei(source, AL_BYTE_OFFSET, (ALint)(start_offset - (start_offset % 4)));
    alSourcePlay(source);
    if (alGetError() != AL_NO_ERROR) {
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        alDeleteSources(1, &source);
        alDeleteBuffers(1, &buffer);
        fprintf(stderr, "[host] audio experiment: OpenAL source did not start\n");
        fflush(stderr);
        return 0;
    }
    v.live = true;
    v.source = source;
    v.buffer = buffer;
    v.bytes = bytes;
    fprintf(stderr,
            "[host] audio experiment: DirectSound buffer %u is playing through OpenAL Soft, "
            "48000 Hz stereo 16-bit, %u bytes\n",
            id, bytes);
    fflush(stderr);
    return 1;
#endif
}

void host_openal_ring_stop(uint32_t id) {
#if !defined(__APPLE__)
    (void)id;
#else
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_ready)
        return;
    auto it = g_voices.find(id);
    if (it == g_voices.end())
        return;
    bind_context();
    release_voice_locked(it->second);
    g_voices.erase(it);
#endif
}

void host_openal_ring_volume(uint32_t id, int32_t volume_mb) {
#if !defined(__APPLE__)
    (void)id;
    (void)volume_mb;
#else
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_voices.find(id);
    if (!g_ready || it == g_voices.end() || !it->second.live)
        return;
    bind_context();
    alSourcef(it->second.source, AL_GAIN, gain_from_mb(volume_mb));
#endif
}

uint32_t host_openal_ring_position(uint32_t id) {
#if !defined(__APPLE__)
    (void)id;
    return 0;
#else
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_voices.find(id);
    if (!g_ready || it == g_voices.end())
        return 0;
    return position_locked(it->second);
#endif
}

uint32_t host_openal_ring_write_position(uint32_t id) {
#if !defined(__APPLE__)
    (void)id;
    return 0;
#else
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_voices.find(id);
    if (!g_ready || it == g_voices.end() || !it->second.live)
        return 0;
    uint32_t play = position_locked(it->second);
    uint32_t refresh = g_refresh > 0 ? (uint32_t)g_refresh : 50u;
    uint32_t lead = (48000u / refresh) * 4u;
    return (play + lead) % it->second.bytes;
#endif
}

int host_openal_ring_owns(uint32_t id) {
#if !defined(__APPLE__)
    (void)id;
    return 0;
#else
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_voices.find(id);
    return it != g_voices.end() && it->second.live ? 1 : 0;
#endif
}
