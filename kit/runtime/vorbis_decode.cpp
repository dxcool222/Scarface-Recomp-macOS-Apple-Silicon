// Scarface in-game audio is Ogg Vorbis. The guest calls the Xiph C API and
// reads one field of the 32-bit OggVorbis_File itself (links, at vf+0x34).
// The decoder object stays on the host. Guest memory only receives that
// links count and a small vorbis_info the guest reads for channels and rate.
#include "vorbis_decode.h"

#include "guest.h"
#include "imports.h"
#include "memory.h"
#include "win32.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#define OV_EXCLUDE_STATIC_CALLBACKS
#include <vorbis/vorbisfile.h>

constexpr uint32_t k_links_offset = 0x34;
constexpr uint32_t k_max_bytes = 64u * 1024u * 1024u;

struct MemSrc {
    const uint8_t *data = nullptr;
    size_t size = 0;
    size_t pos = 0;
};

struct VorbisClip {
    std::vector<uint8_t> bytes;
    MemSrc src;
    OggVorbis_File file{};
    bool open = false;
    int channels = 0;
    int rate = 0;
    uint64_t samples = 0;
    bool at_end = false;
};

extern "C" {
static size_t mem_read(void *ptr, size_t size, size_t nmemb, void *datasource) {
    auto *m = static_cast<MemSrc *>(datasource);
    if (!m || !ptr || size == 0 || nmemb == 0)
        return 0;
    size_t have = m->pos < m->size ? m->size - m->pos : 0;
    size_t got = have / size;
    if (got > nmemb)
        got = nmemb;
    size_t bytes = got * size;
    if (bytes)
        std::memcpy(ptr, m->data + m->pos, bytes);
    m->pos += bytes;
    return got;
}

static int mem_seek(void *datasource, ogg_int64_t offset, int whence) {
    auto *m = static_cast<MemSrc *>(datasource);
    if (!m)
        return -1;
    int64_t base = 0;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = (int64_t)m->pos;
    else if (whence == SEEK_END)
        base = (int64_t)m->size;
    else
        return -1;
    int64_t next = base + offset;
    if (next < 0 || (uint64_t)next > m->size)
        return -1;
    m->pos = (size_t)next;
    return 0;
}

static long mem_tell(void *datasource) {
    auto *m = static_cast<MemSrc *>(datasource);
    return m ? (long)m->pos : -1;
}
}

static void vlog(const char *fmt, ...) {
    char body[384];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    LOGW("t=%u %s", host_millis(), body);
    fflush(stderr);
}

int vorbis_clip_open(const uint8_t *data, size_t size, VorbisClip **out) {
    if (out)
        *out = nullptr;
    if (!out || !data || size < 4 || size > k_max_bytes)
        return OV_EINVAL;
    if (std::memcmp(data, "OggS", 4) != 0)
        return OV_ENOTVORBIS;
    auto *clip = new VorbisClip();
    clip->bytes.assign(data, data + size);
    clip->src.data = clip->bytes.data();
    clip->src.size = clip->bytes.size();
    ov_callbacks cb{};
    cb.read_func = mem_read;
    cb.seek_func = mem_seek;
    cb.close_func = nullptr;
    cb.tell_func = mem_tell;
    int rc = ov_open_callbacks(&clip->src, &clip->file, nullptr, 0, cb);
    if (rc != 0) {
        delete clip;
        return rc;
    }
    clip->open = true;
    vorbis_info *info = ov_info(&clip->file, -1);
    ogg_int64_t samples = ov_pcm_total(&clip->file, -1);
    if (!info || info->channels <= 0 || info->channels > 8 || info->rate <= 0 || samples < 0) {
        ov_clear(&clip->file);
        delete clip;
        return OV_EBADHEADER;
    }
    clip->channels = info->channels;
    clip->rate = (int)info->rate;
    clip->samples = (uint64_t)samples;
    *out = clip;
    return 0;
}

int vorbis_clip_channels(const VorbisClip *clip) {
    return clip ? clip->channels : 0;
}
int vorbis_clip_rate(const VorbisClip *clip) {
    return clip ? clip->rate : 0;
}
uint64_t vorbis_clip_pcm_total(const VorbisClip *clip) {
    return clip ? clip->samples : 0;
}

int vorbis_clip_read(VorbisClip *clip, uint8_t *dst, int nbytes) {
    if (!clip || !clip->open || !dst || nbytes < 0)
        return OV_EFAULT;
    if (clip->at_end || nbytes == 0)
        return 0;
    int frame = clip->channels * 2;
    if (frame <= 0)
        return OV_EBADHEADER;
    int bytes = nbytes - (nbytes % frame);
    if (bytes <= 0)
        return 0;
    int bitstream = 0;
    long got = ov_read(&clip->file, reinterpret_cast<char *>(dst), bytes, 0, 2, 1, &bitstream);
    if (got < 0)
        return (int)got;
    if (got == 0)
        clip->at_end = true;
    return (int)got;
}

int vorbis_clip_seek(VorbisClip *clip, uint64_t sample) {
    if (!clip || !clip->open)
        return OV_EFAULT;
    if (sample > clip->samples)
        return OV_EINVAL;
    clip->at_end = sample == clip->samples;
    if (clip->at_end)
        return 0;
    int rc = ov_pcm_seek(&clip->file, (ogg_int64_t)sample);
    if (rc != 0)
        return rc;
    clip->at_end = false;
    return 0;
}

void vorbis_clip_close(VorbisClip *clip) {
    if (!clip)
        return;
    if (clip->open)
        ov_clear(&clip->file);
    delete clip;
}

struct GuestStream {
    VorbisClip *clip = nullptr;
    uint32_t info = 0;
    bool logged_pcm = false;
};

static std::mutex g_streams_mu;
static std::unordered_map<uint32_t, GuestStream> g_streams;

static void forget(uint32_t vf) {
    auto it = g_streams.find(vf);
    if (it == g_streams.end())
        return;
    if (it->second.info)
        heap_free(it->second.info);
    vorbis_clip_close(it->second.clip);
    g_streams.erase(it);
}

static int peak_i16(const uint8_t *p, int n) {
    int peak = 0;
    int samples = n / 2;
    for (int i = 0; i < samples; i++) {
        int16_t s;
        std::memcpy(&s, p + i * 2, 2);
        int a = s < 0 ? -s : s;
        if (a > peak)
            peak = a;
    }
    return peak;
}

static bool copy_resource(uint32_t datasource, uint32_t initial, uint32_t ibytes,
                          std::vector<uint8_t> *out) {
    if (ibytes) {
        if (ibytes > k_max_bytes || !initial || !gm_valid(initial, ibytes))
            return false;
        out->assign(gm_ptr(initial), gm_ptr(initial) + ibytes);
    }
    if (!datasource || !gm_valid(datasource + 0x18, 4))
        return false;
    uint32_t size = rd32(datasource + 0x14);
    uint32_t ptr = rd32(datasource + 0x18);
    if (!ptr || !size || size > k_max_bytes || !gm_valid(ptr, size))
        return false;
    out->insert(out->end(), gm_ptr(ptr), gm_ptr(ptr) + size);
    return !out->empty() && out->size() <= k_max_bytes;
}

void vorbis_ov_open_callbacks(X86 *c) {
    uint32_t datasource = arg(c, 0);
    uint32_t vf = arg(c, 1);
    uint32_t initial = arg(c, 2);
    uint32_t ibytes = arg(c, 3);
    std::vector<uint8_t> bytes;
    if (!vf || !gm_valid(vf + k_links_offset, 4) || !copy_resource(datasource, initial, ibytes, &bytes)) {
        vlog("vorbis: open vf=%08x failed: resource unreadable", vf);
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    VorbisClip *clip = nullptr;
    int rc = vorbis_clip_open(bytes.data(), bytes.size(), &clip);
    if (rc != 0) {
        vlog("vorbis: open vf=%08x failed: %d bytes=%zu magic=%08x", vf, rc, bytes.size(),
             bytes.size() >= 4 ? (uint32_t)(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24)
                               : 0);
        set_eax(c, (uint32_t)rc);
        return;
    }
    uint32_t info = heap_alloc(32, true);
    if (!info) {
        vorbis_clip_close(clip);
        vlog("vorbis: open vf=%08x failed: no guest memory for vorbis_info", vf);
        set_eax(c, (uint32_t)OV_EFAULT);
        return;
    }
    wr32(info + 0, 0);
    wr32(info + 4, (uint32_t)vorbis_clip_channels(clip));
    wr32(info + 8, (uint32_t)vorbis_clip_rate(clip));
    std::lock_guard<std::mutex> lock(g_streams_mu);
    forget(vf);
    g_streams[vf] = GuestStream{clip, info, false};
    wr32(vf + k_links_offset, 1);
    vlog("vorbis: libvorbisfile open vf=%08x bytes=%zu ch=%d rate=%d samples=%llu", vf, bytes.size(),
         vorbis_clip_channels(clip), vorbis_clip_rate(clip),
         (unsigned long long)vorbis_clip_pcm_total(clip));
    set_eax(c, 0);
}

void vorbis_ov_info(X86 *c) {
    uint32_t vf = arg(c, 0);
    int32_t link = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lock(g_streams_mu);
    auto it = g_streams.find(vf);
    if (it == g_streams.end() || (link != 0 && link != -1)) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, it->second.info);
}

void vorbis_ov_pcm_total(X86 *c) {
    uint32_t vf = arg(c, 0);
    int32_t link = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lock(g_streams_mu);
    auto it = g_streams.find(vf);
    uint64_t n = 0;
    if (it != g_streams.end() && (link == 0 || link == -1))
        n = vorbis_clip_pcm_total(it->second.clip);
    set_eax64(c, n);
}

void vorbis_ov_pcm_seek(X86 *c) {
    uint32_t vf = arg(c, 0);
    uint64_t sample = (uint64_t)arg(c, 1) | ((uint64_t)arg(c, 2) << 32);
    std::lock_guard<std::mutex> lock(g_streams_mu);
    auto it = g_streams.find(vf);
    if (it == g_streams.end()) {
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    int rc = vorbis_clip_seek(it->second.clip, sample);
    if (rc != 0)
        vlog("vorbis: seek vf=%08x sample=%llu failed %d", vf, (unsigned long long)sample, rc);
    set_eax(c, (uint32_t)rc);
}

void vorbis_ov_read(X86 *c) {
    uint32_t vf = arg(c, 0);
    uint32_t buffer = arg(c, 1);
    int32_t length = (int32_t)arg(c, 2);
    uint32_t bigendian = arg(c, 3);
    uint32_t word = arg(c, 4);
    uint32_t sgned = arg(c, 5);
    uint32_t bitstream = arg(c, 6);
    if (bigendian != 0 || word != 2 || sgned != 1 || length < 0) {
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    std::lock_guard<std::mutex> lock(g_streams_mu);
    auto it = g_streams.find(vf);
    if (it == g_streams.end()) {
        set_eax(c, (uint32_t)OV_EINVAL);
        return;
    }
    if (length == 0) {
        set_eax(c, 0);
        return;
    }
    if (!buffer || !gm_valid(buffer, (uint32_t)length)) {
        set_eax(c, (uint32_t)OV_EFAULT);
        return;
    }
    std::vector<uint8_t> pcm((size_t)length);
    int n = vorbis_clip_read(it->second.clip, pcm.data(), length);
    if (n < 0) {
        vlog("vorbis: read vf=%08x failed %d", vf, n);
        set_eax(c, (uint32_t)n);
        return;
    }
    if (n > 0) {
        std::memcpy(gm_ptr(buffer), pcm.data(), (size_t)n);
        if (!it->second.logged_pcm) {
            it->second.logged_pcm = true;
            vlog("vorbis: read vf=%08x bytes=%d peak=%d", vf, n, peak_i16(pcm.data(), n));
        }
    }
    if (bitstream && gm_valid(bitstream, 4))
        wr32(bitstream, 0);
    set_eax(c, (uint32_t)n);
}

void vorbis_ov_clear(X86 *c) {
    uint32_t vf = arg(c, 0);
    std::lock_guard<std::mutex> lock(g_streams_mu);
    forget(vf);
    if (vf && gm_valid(vf + k_links_offset, 4))
        wr32(vf + k_links_offset, 0);
    set_eax(c, 0);
}
