// board_map.h -- physical wiring maps, pure and host-testable
//
// canonical square: sq = rank*8 + file, a1=0 .. h8=63 (file 0=a..7=h, rank 0..7)
//
// both maps below come from the validated single-chain detection sketch
//
// sensor read (one 64-bit HC165 daisy chain on SR_MISO, bit-banged):
//   bits clock out in canonical order a1,b1..h1, a2..h2, ... a8..h8
//   so read-index i maps straight to square i, no math needed
//   A3144 is active-low, a LOW bit means a piece is on the square
//
// LED chain (WS2812B DOUT from the ESP):
//   LED 0 = h8, data runs right to left across each rank (h..a), then drops
//   to the rank below: h8,g8..a8, h7..a7, ... h1..a1
//   led = (7 - rank)*8 + (7 - file)

#ifndef CHESSLINK_BOARD_MAP_H
#define CHESSLINK_BOARD_MAP_H

// canonical square -> position in the WS2812B chain (0 = first LED = h8)
static inline int cl_led_index(int sq) {
    int file = sq & 7, rank = sq >> 3;
    return (7 - rank) * 8 + (7 - file);
}

// read-index in the shift-register bitstream -> canonical square
// the chain clocks out in canonical order, so this is the identity map
// (read-index 0 = first bit clocked out = a1 = sq 0)
static inline int cl_sensor_sq(int readidx) {
    return readidx;
}

#endif
