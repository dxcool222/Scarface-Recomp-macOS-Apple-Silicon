#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// OpenAL Soft playback of Scarface's one 384000-byte mix ring.
// On unless RECOMP_AUDIO_OPENAL is 0, off, false, or no.
// The SDL float ring remains the path for every other buffer.

int host_openal_ring_enabled(void);

// Points one looping OpenAL buffer at `pcm` and starts it. Returns 0 if this
// channel is not on the OpenAL path. `pcm` must stay allocated until stop.
int host_openal_ring_play(uint32_t id, const void *pcm, uint32_t bytes, uint32_t start_offset,
                          int32_t volume_mb);

void host_openal_ring_stop(uint32_t id);
void host_openal_ring_volume(uint32_t id, int32_t volume_mb);

// Play cursor in bytes, already wrapped to the ring. 0 if this buffer is not
// an OpenAL ring.
uint32_t host_openal_ring_position(uint32_t id);
// Write cursor: play plus one OpenAL refresh period, wrapped. DSOAL's rule.
uint32_t host_openal_ring_write_position(uint32_t id);
int host_openal_ring_owns(uint32_t id);

#ifdef __cplusplus
}
#endif
