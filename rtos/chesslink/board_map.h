// board_map.h -- physical wiring map for the LED chain, pure and host-testable
//
// canonical square: sq = rank*8 + file, a1=0 .. h8=63 (file 0=a..7=h, rank 0..7)
//
// (the sensor read map is the identity -- the HC165 chain clocks out in canonical
// order a1,b1..h8 -- and lives inline in task_sensor.cpp's sr_read_all)
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

#endif
