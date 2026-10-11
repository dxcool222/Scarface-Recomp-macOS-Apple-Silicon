#pragma once

#include <stddef.h>
#include <stdint.h>

struct X86;

// One open Ogg Vorbis stream. The pointer stays valid until vorbis_clip_close.
struct VorbisClip;

// 0 on success. Negative values are the Xiph ov_open_callbacks error codes.
int vorbis_clip_open(const uint8_t *data, size_t size, VorbisClip **out);
int vorbis_clip_channels(const VorbisClip *clip);
int vorbis_clip_rate(const VorbisClip *clip);
// PCM samples per channel, which is what ov_pcm_total returns.
uint64_t vorbis_clip_pcm_total(const VorbisClip *clip);
// Interleaved 16-bit signed little-endian PCM. Returns the bytes written,
// 0 at end of stream, or a negative Xiph error.
int vorbis_clip_read(VorbisClip *clip, uint8_t *dst, int nbytes);
// sample is an index in samples per channel. 0 on success.
int vorbis_clip_seek(VorbisClip *clip, uint64_t sample);
void vorbis_clip_close(VorbisClip *clip);

void vorbis_ov_open_callbacks(X86 *c);
void vorbis_ov_info(X86 *c);
void vorbis_ov_pcm_total(X86 *c);
void vorbis_ov_pcm_seek(X86 *c);
void vorbis_ov_read(X86 *c);
void vorbis_ov_clear(X86 *c);
