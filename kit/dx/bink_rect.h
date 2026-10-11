// Destination addressing for BinkCopyToBufferRect. The destination is always
// the lowest address of a buffer `height` rows tall; a negative pitch stores
// the image bottom-up inside that same buffer rather than below it.
#pragma once
#include <cstdint>
#include <cstdlib>

struct BinkRectRows {
    int64_t first = 0; // address of destination row dy
    int64_t low = 0, high = 0; // every byte written lies in [low, high)
};

inline bool bink_rect_rows(int64_t dest, int64_t pitch, uint64_t height, uint64_t dx,
                           uint64_t dy, uint64_t row_bytes, uint64_t rows, uint64_t bpp,
                           BinkRectRows *out) {
    uint64_t stride = (uint64_t)std::llabs(pitch);
    if (!dest || !pitch || !row_bytes || !rows || !height || dy + rows > height ||
        dx * bpp + row_bytes > stride)
        return false;
    int64_t top = pitch < 0 ? dest + (int64_t)((height - 1) * stride) : dest;
    int64_t first = top + (int64_t)dy * pitch + (int64_t)(dx * bpp);
    int64_t last = first + (int64_t)(rows - 1) * pitch;
    out->first = first;
    out->low = first < last ? first : last;
    out->high = (first < last ? last : first) + (int64_t)row_bytes;
    return true;
}
