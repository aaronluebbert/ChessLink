// chesslink_tracker.cpp -- presence-only move inference
//
// the problem
// a reed/hall board reports a 64-bit occupancy word and nothing else. a quiet
// move shows up as "from emptied, to filled". but a capture only shows "from
// emptied" (the destination was already occupied and stays occupied), so the
// occupancy alone cannot tell you WHERE the piece landed, and two captures from
// the same square look identical. en passant clears a square the piece never
// touches, castling moves two pieces at once, and promotion cannot reveal which
// piece the pawn became. so occupancy diffing by itself is not enough.
//
// the approach
// 1. hold a committed position we trust (start pos, or synced from lichess)
// 2. debounce, or trust the caller's debounce with stable_needed = 1
// 3. on every reading accumulate ever_lifted, the union of every square that
//    has read empty since the current move began. this catches the transient
//    where the human lifts the piece being captured, which disambiguates it
// 4. when a reading is stable and differs from committed, ask the engine for
//    all legal moves and keep those whose result-occupancy matches. quiet,
//    double-push, en passant and castling each give a unique occupancy so they
//    resolve directly. captures from one origin tie on destination (broken with
//    ever_lifted) and promotions tie on piece (broken with promo_choice)

#include "chesslink_tracker.h"

void clt_init(ChessLinkTracker *t, uint8_t stable_needed) {
    cl_init_startpos(&t->game);
    t->committed_occ = cl_occupancy(&t->game);
    t->ever_lifted = 0;
    t->last_raw = t->committed_occ;
    t->stable_count = 0;
    t->stable_needed = stable_needed ? stable_needed : 1;
    t->promo_choice = CL_QUEEN;
    t->last_move.from = t->last_move.to = 0;
    t->last_move.promo = t->last_move.flags = 0;
}

void clt_set_promotion(ChessLinkTracker *t, uint8_t piece_type) {
    if (piece_type >= CL_KNIGHT && piece_type <= CL_QUEEN)
        t->promo_choice = piece_type;
}

bool clt_sync_fen(ChessLinkTracker *t, const char *fen) {
    if (!cl_from_fen(&t->game, fen)) return false;
    t->committed_occ = cl_occupancy(&t->game);
    t->ever_lifted = 0;
    t->last_raw = t->committed_occ;
    t->stable_count = 0;
    return true;
}

// apply a known move (opponent / lichess) to committed truth, keeping sync
bool clt_apply_move(ChessLinkTracker *t, int from, int to, int promo) {
    Move legal[CL_MAX_MOVES];
    int n = cl_gen_legal(&t->game, legal);
    const Move *m = cl_find_move(legal, n, from, to, promo);
    if (!m) return false;
    t->last_move = *m;
    cl_make_move(&t->game, m);
    t->committed_occ = cl_occupancy(&t->game);
    t->ever_lifted = 0;
    t->last_raw = t->committed_occ;
    t->stable_count = 0;
    return true;
}

bool clt_apply_uci(ChessLinkTracker *t, const char *uci) {
    if (!uci || uci[0] < 'a' || uci[0] > 'h') return false;
    int from = cl_sq(uci[0] - 'a', uci[1] - '1');
    int to   = cl_sq(uci[2] - 'a', uci[3] - '1');
    int promo = 0;
    switch (uci[4]) {                 // 5th char present only on promotions
        case 'q': promo = CL_QUEEN;  break;
        case 'r': promo = CL_ROOK;   break;
        case 'b': promo = CL_BISHOP; break;
        case 'n': promo = CL_KNIGHT; break;
        default:  promo = 0;         break;
    }
    return clt_apply_move(t, from, to, promo);
}

uint64_t clt_expected_occupancy(const ChessLinkTracker *t) {
    return t->committed_occ;
}

void clt_move_to_uci(const Move *m, char *out) {
    out[0] = 'a' + cl_file(m->from);
    out[1] = '1' + cl_rank(m->from);
    out[2] = 'a' + cl_file(m->to);
    out[3] = '1' + cl_rank(m->to);
    if (m->promo) {
        char c = 'q';
        if (m->promo == CL_ROOK) c = 'r';
        else if (m->promo == CL_BISHOP) c = 'b';
        else if (m->promo == CL_KNIGHT) c = 'n';
        out[4] = c;
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

// advance the game and reset the move trace
static void commit(ChessLinkTracker *t, const Move *m) {
    t->last_move = *m;
    cl_make_move(&t->game, m);
    t->committed_occ = cl_occupancy(&t->game);
    t->ever_lifted = 0;
}

ClTrackEvent clt_update(ChessLinkTracker *t, uint64_t sensor_occ) {
    // accumulate the lift trace on the raw reading, before the debounce gate,
    // so a brief lift of a captured piece is never missed
    t->ever_lifted |= (t->committed_occ & ~sensor_occ);

    // debounce. stable_needed == 1 resolves on the first reading, which is what
    // we want when task_SensorScan has already debounced upstream
    if (sensor_occ == t->last_raw) {
        if (t->stable_count < 255) t->stable_count++;
    } else {
        t->last_raw = sensor_occ;
        t->stable_count = 1;
    }
    if (t->stable_count < t->stable_needed) return CL_EV_NONE;

    // board back to the trusted position, any in-progress move was undone
    if (sensor_occ == t->committed_occ) {
        t->ever_lifted = 0;
        return CL_EV_SETTLED;
    }

    Move legal[CL_MAX_MOVES];
    int nlegal = cl_gen_legal(&t->game, legal);

    // candidates: legal moves whose resulting occupancy matches the sensor.
    // they all share one from-square (a different from clears a different bit),
    // so only 'to' and 'promo' can vary between them
    int cand[CL_MAX_MOVES];
    int nc = 0;
    for (int i = 0; i < nlegal; i++)
        if (cl_result_occupancy(&t->game, &legal[i]) == sensor_occ)
            cand[nc++] = i;

    if (nc == 0) {
        // nothing matches yet. tell "mid-move" from "illegal": a piece now on a
        // square that was empty and is not any legal destination is illegal
        uint64_t extra = sensor_occ & ~t->committed_occ;
        if (extra) {
            uint64_t legal_dests = 0;
            for (int i = 0; i < nlegal; i++)
                legal_dests |= (uint64_t)1 << legal[i].to;
            if (extra & ~legal_dests) return CL_EV_ILLEGAL;
        }
        return CL_EV_IN_PROGRESS;
    }

    if (nc == 1) { commit(t, &legal[cand[0]]); return CL_EV_MOVE; }

    // multiple candidates: resolve destination first, then promotion piece
    int to_final = legal[cand[0]].to;
    bool one_dest = true;
    for (int k = 1; k < nc; k++)
        if (legal[cand[k]].to != to_final) { one_dest = false; break; }

    if (!one_dest) {
        // capture-destination ambiguity: the real target is the enemy square
        // that was lifted (captured piece removed) and is now filled again
        to_final = -1;
        for (int k = 0; k < nc; k++) {
            uint64_t tobit = (uint64_t)1 << legal[cand[k]].to;
            if ((t->ever_lifted & tobit) && (sensor_occ & tobit)) {
                if (to_final >= 0 && to_final != (int)legal[cand[k]].to) {
                    to_final = -2; break;      // genuinely can't tell
                }
                to_final = legal[cand[k]].to;
            }
        }
        if (to_final < 0) return CL_EV_AMBIGUOUS;
    }

    // among candidates landing on to_final, any leftover difference is the
    // promotion piece, so pick the configured choice (default queen)
    int chosen = -1;
    for (int k = 0; k < nc; k++) {
        if (legal[cand[k]].to != to_final) continue;
        if (chosen < 0) chosen = cand[k];
        if (legal[cand[k]].promo == t->promo_choice) { chosen = cand[k]; break; }
        if (legal[cand[k]].promo == 0) { chosen = cand[k]; break; }
    }
    if (chosen < 0) return CL_EV_AMBIGUOUS;
    commit(t, &legal[chosen]);
    return CL_EV_MOVE;
}
