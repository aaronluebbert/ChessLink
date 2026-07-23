#ifndef CHESSLINK_TRACKER_H
#define CHESSLINK_TRACKER_H
#include "chess.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CL_EV_NONE = 0,
    CL_EV_SETTLED,
    CL_EV_IN_PROGRESS,
    CL_EV_MOVE,
    CL_EV_AMBIGUOUS,
    CL_EV_ILLEGAL
} ClTrackEvent;

typedef struct {
    GameState game;
    uint64_t  committed_occ;
    uint64_t  ever_lifted;
    uint64_t  last_raw;
    uint8_t   stable_count;
    uint8_t   stable_needed;
    uint8_t   promo_choice;
    Move      last_move;
} ChessLinkTracker;

void clt_init(ChessLinkTracker *t, uint8_t stable_needed);
void clt_set_promotion(ChessLinkTracker *t, uint8_t piece_type);
bool clt_sync_fen(ChessLinkTracker *t, const char *fen);
bool clt_apply_move(ChessLinkTracker *t, int from, int to, int promo);
bool clt_apply_uci(ChessLinkTracker *t, const char *uci);
uint64_t clt_expected_occupancy(const ChessLinkTracker *t);
void clt_move_to_uci(const Move *m, char *out);
ClTrackEvent clt_update(ChessLinkTracker *t, uint64_t sensor_occ);

#ifdef __cplusplus
}
#endif
#endif
