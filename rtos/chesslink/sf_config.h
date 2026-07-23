// sf_config.h -- Stockfish game presets, shared by the display picker and the
// network task. ported from the Latest branch (CONNECT_ITALL Stockfish screen).

#ifndef CHESSLINK_SF_CONFIG_H
#define CHESSLINK_SF_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int limit; int inc; const char *label; } SfTimePreset;

extern const SfTimePreset SF_TIME_PRESETS[];
extern const int          SF_TIME_COUNT;
extern const char *const  SF_COLOR_LABELS[3];   // "White" / "Black" / "Random"
extern const char *const  SF_COLOR_VALUES[3];   // "white" / "black" / "random"

#ifdef __cplusplus
}
#endif

#endif
