// Decoder contract test. Does not launch the game and does not modify assets.
#include "vorbis_decode.h"

#include "guest.h"
#include "memory.h"
#include "x86.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static int g_fails = 0;

static void expect(bool ok, const char *what) {
    if (ok) {
        std::printf("PASS %s\n", what);
        return;
    }
    std::printf("FAIL %s\n", what);
    g_fails++;
}

static std::vector<uint8_t> load_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static int peak_i16(const uint8_t *p, int n) {
    int peak = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        int16_t s;
        std::memcpy(&s, p + i, 2);
        int a = s < 0 ? -s : s;
        if (a > peak)
            peak = a;
    }
    return peak;
}

static void decode_all(VorbisClip *clip, std::vector<uint8_t> *pcm, int *peak) {
    uint8_t buf[4096];
    *peak = 0;
    for (;;) {
        int n = vorbis_clip_read(clip, buf, (int)sizeof buf);
        if (n < 0) {
            std::printf("read error %d\n", n);
            g_fails++;
            return;
        }
        if (n == 0)
            return;
        int p = peak_i16(buf, n);
        if (p > *peak)
            *peak = p;
        pcm->insert(pcm->end(), buf, buf + n);
    }
}

static void check_clip(const char *label, const std::vector<uint8_t> &ogg, int want_ch, int want_rate,
                       int min_peak) {
    VorbisClip *clip = nullptr;
    int rc = vorbis_clip_open(ogg.data(), ogg.size(), &clip);
    expect(rc == 0 && clip, label);
    if (!clip)
        return;
    char msg[160];
    std::snprintf(msg, sizeof msg, "%s channels %d", label, vorbis_clip_channels(clip));
    expect(vorbis_clip_channels(clip) == want_ch, msg);
    std::snprintf(msg, sizeof msg, "%s rate %d", label, vorbis_clip_rate(clip));
    expect(vorbis_clip_rate(clip) == want_rate, msg);
    uint64_t samples = vorbis_clip_pcm_total(clip);
    std::snprintf(msg, sizeof msg, "%s samples %llu", label, (unsigned long long)samples);
    expect(samples > 0, msg);
    std::vector<uint8_t> pcm;
    int peak = 0;
    decode_all(clip, &pcm, &peak);
    int expect_bytes = (int)samples * want_ch * 2;
    std::snprintf(msg, sizeof msg, "%s pcm bytes %zu expected %d peak %d", label, pcm.size(), expect_bytes,
                  peak);
    expect((int)pcm.size() > 0 && (int)pcm.size() >= expect_bytes - want_ch * 2 * 4, msg);
    expect(peak >= min_peak, msg);
    int eof = vorbis_clip_read(clip, pcm.data(), 64);
    expect(eof == 0, "eof read is 0");
    uint64_t mid = samples / 2;
    int seek = vorbis_clip_seek(clip, mid);
    std::snprintf(msg, sizeof msg, "%s seek mid rc %d", label, seek);
    expect(seek == 0, msg);
    uint8_t window[2048];
    int got = vorbis_clip_read(clip, window, (int)sizeof window);
    int mid_peak = got > 0 ? peak_i16(window, got) : 0;
    std::snprintf(msg, sizeof msg, "%s mid read %d peak %d", label, got, mid_peak);
    expect(got > 0 && mid_peak >= min_peak, msg);
    int bad = vorbis_clip_seek(clip, samples + 8);
    expect(bad != 0, "seek past end fails");
    vorbis_clip_close(clip);
}

static void check_guest(const std::vector<uint8_t> &ogg, uint32_t want_ch, uint32_t want_rate) {
    mem_init();
    uint32_t payload = heap_alloc((uint32_t)ogg.size(), false, 16);
    std::memcpy(gm_ptr(payload), ogg.data(), ogg.size());
    uint32_t ds = heap_alloc(32, true, 16);
    wr32(ds + 0x10, 0);
    wr32(ds + 0x14, (uint32_t)ogg.size());
    wr32(ds + 0x18, payload);
    uint32_t vf = heap_alloc(256, true, 16);
    uint32_t stack = heap_alloc(256, true, 16);
    X86 cpu{};
    cpu.r[4] = stack;
    auto push_arg = [&](int slot, uint32_t value) { wr32(stack + 4 + slot * 4, value); };
    push_arg(0, ds);
    push_arg(1, vf);
    push_arg(2, 0);
    push_arg(3, 0);
    vorbis_ov_open_callbacks(&cpu);
    uint32_t eax = cpu.r[0];
    char msg[128];
    std::snprintf(msg, sizeof msg, "guest open eax %08x links %u", eax, rd32(vf + 0x34));
    expect(eax == 0 && rd32(vf + 0x34) == 1, msg);
    push_arg(0, vf);
    push_arg(1, 0);
    vorbis_ov_info(&cpu);
    uint32_t info = cpu.r[0];
    std::snprintf(msg, sizeof msg, "guest info %08x ch %u rate %u", info, info ? rd32(info + 4) : 0,
                  info ? rd32(info + 8) : 0);
    expect(info != 0 && rd32(info + 4) == want_ch && rd32(info + 8) == want_rate, msg);
    push_arg(1, 0xffffffffu);
    vorbis_ov_pcm_total(&cpu);
    expect(cpu.r[0] > 0, "guest pcm total");
    uint32_t out = heap_alloc(4096, true, 16);
    uint32_t bitstream = heap_alloc(4, true, 16);
    push_arg(0, vf);
    push_arg(1, out);
    push_arg(2, 4096);
    push_arg(3, 0);
    push_arg(4, 2);
    push_arg(5, 1);
    push_arg(6, bitstream);
    vorbis_ov_read(&cpu);
    int n = (int)cpu.r[0];
    int peak = n > 0 ? peak_i16(gm_ptr(out), n) : 0;
    std::snprintf(msg, sizeof msg, "guest read %d peak %d", n, peak);
    expect(n > 0 && peak > 1000, msg);
    push_arg(0, vf);
    vorbis_ov_clear(&cpu);
    expect(rd32(vf + 0x34) == 0, "guest clear drops links");
}

int main(int argc, char **argv) {
    if (argc == 4 && std::strcmp(argv[1], "--dump") == 0) {
        auto ogg = load_file(argv[2]);
        VorbisClip *clip = nullptr;
        if (vorbis_clip_open(ogg.data(), ogg.size(), &clip) != 0 || !clip)
            return 2;
        std::vector<uint8_t> pcm;
        int peak = 0;
        decode_all(clip, &pcm, &peak);
        vorbis_clip_close(clip);
        std::ofstream out(argv[3], std::ios::binary);
        out.write(reinterpret_cast<const char *>(pcm.data()), (std::streamsize)pcm.size());
        std::printf("decoded %zu bytes, peak %d\n", pcm.size(), peak);
        return g_fails || !out ? 1 : 0;
    }
    const char *stereo = argc > 1 ? argv[1] : "/tmp/scarface-sine-stereo.ogg";
    const char *retail = argc > 2 ? argv[2] : "/tmp/scarface-sfx-6111.ogg";
    auto b = load_file(stereo);
    auto c = load_file(retail);
    expect(b.size() > 32 && c.size() > 32, "test ogg files present");
    if (b.empty() || c.empty())
        return 1;
    check_clip("stereo sine", b, 2, 48000, 1000);
    check_clip("retail sfx", c, 1, 48000, 1);
    VorbisClip *junk = nullptr;
    const uint8_t garbage[16] = {1, 2, 3, 4};
    expect(vorbis_clip_open(garbage, sizeof garbage, &junk) != 0 && junk == nullptr, "garbage rejected");
    VorbisClip *one = nullptr;
    VorbisClip *two = nullptr;
    expect(vorbis_clip_open(c.data(), c.size(), &one) == 0, "clip one");
    expect(vorbis_clip_open(b.data(), b.size(), &two) == 0, "clip two");
    expect(vorbis_clip_channels(one) == 1 && vorbis_clip_channels(two) == 2, "clips stay independent");
    vorbis_clip_close(one);
    expect(vorbis_clip_channels(two) == 2 && vorbis_clip_rate(two) == 48000, "closing one keeps the other");
    uint8_t buf[256];
    expect(vorbis_clip_read(two, buf, 256) > 0, "survivor still decodes");
    vorbis_clip_close(two);
    check_guest(b, 2, 48000);
    VorbisClip *retail_clip = nullptr;
    int rc = vorbis_clip_open(c.data(), c.size(), &retail_clip);
    expect(rc == 0 && retail_clip, "retail clip opens");
    if (retail_clip) {
        std::printf("retail ch=%d rate=%d samples=%llu\n", vorbis_clip_channels(retail_clip),
                    vorbis_clip_rate(retail_clip),
                    (unsigned long long)vorbis_clip_pcm_total(retail_clip));
        std::vector<uint8_t> pcm;
        int peak = 0;
        decode_all(retail_clip, &pcm, &peak);
        std::printf("retail pcm_bytes=%zu peak=%d\n", pcm.size(), peak);
        expect(!pcm.empty() && peak > 0, "retail clip is not silent");
        vorbis_clip_close(retail_clip);
    }
    return g_fails ? 1 : 0;
}
