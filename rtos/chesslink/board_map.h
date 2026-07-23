// board_map.h -- physical wiring maps, pure and host-testable
//
// canonical square: sq = rank*8 + file, a1=0 .. h8=63 (file 0=a..7=h, rank 0..7)
//
// LED chain (WS2812B DOUT from the ESP): h1,h2,h3,h4,h5,h6,h7,h8, g1,g2,...,a8
//   -> files run h,g,f,...,a; within a file, ranks 1..8.
//   led_index(sq): file = sq&7 (0=a..7=h), rank = sq>>3.
//     chain position = (7 - file) * 8 + rank
//
// shift-register read (bit order clocked out of the HC165 chain):
//   h8,h7,h6,h5,h4,h3,h2,h1, g8,g7,...,a1
//   -> files run h,g,...,a; within a file, ranks 8..1.
//   sensor_sq(readidx): file = 7 - (readidx/8), rank = 7 - (readidx%8)

#ifndef CHESSLINK_BOARD_MAP_H
#define CHESSLINK_BOARD_MAP_H

// canonical square -> position in the WS2812B chain (0 = first LED = h1)
static inline int cl_led_index(int sq) {
    int file = sq & 7, rank = sq >> 3;
    return (7 - file) * 8 + rank;
}

// read-index in the shift-register bitstream -> canonical square
// (read-index 0 = first bit clocked out = h8)
static inline int cl_sensor_sq(int readidx) {
    int file = 7 - (readidx / 8);
    int rank = 7 - (readidx % 8);
    return rank * 8 + file;
}

#endif
