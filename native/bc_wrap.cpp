// C entry point to the vendored BCn decoder (third_party/bc_decoder, MPL-2.0, from Eden/Skyline).
#include "third_party/bc_decoder/bc_decoder.h"
extern "C" void sv_bc_decode(unsigned type, const unsigned char* src, unsigned char* dst, size_t x, size_t y,
                             size_t w, size_t h) {
    switch (type) {
    case 0x1A: bcn::DecodeBc1(src, dst, x, y, w, h); break;
    case 0x1B: bcn::DecodeBc2(src, dst, x, y, w, h); break;
    case 0x1C: bcn::DecodeBc3(src, dst, x, y, w, h); break;
    case 0x1D: bcn::DecodeBc4(src, dst, x, y, w, h, false); break;
    case 0x1E: bcn::DecodeBc5(src, dst, x, y, w, h, false); break;
    case 0x20: bcn::DecodeBc7(src, dst, x, y, w, h); break;
    default: break;
    }
}
