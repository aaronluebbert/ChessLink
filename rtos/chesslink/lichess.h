// lichess.h -- pure helpers for lichess board-api parsing, no Arduino deps
//
// the algorithmic bits of the lichess integration live here so they can be
// host-tested: pulling moves out of the "moves" string, deciding which of them
// are the opponent's (so we never echo our own back), and reading our color.
// the network task does the http + json and calls these.

#ifndef CHESSLINK_LICHESS_H
#define CHESSLINK_LICHESS_H

#ifdef __cplusplus
extern "C" {
#endif

// number of space-separated uci moves in a lichess "moves" string ("" -> 0)
int lichess_move_count(const char *moves);

// copy the idx-th (0-based) uci token into out (>= 6 bytes). false if absent
bool lichess_nth_move(const char *moves, int idx, char *out);

// color that played ply idx: 0 = white, 1 = black
static inline int lichess_ply_color(int idx) { return idx & 1; }

// walk the moves list forward from *applied, skipping our own plies, and return
// the next OPPONENT move into out (>= 6). updates *applied past what it consumed
// (including skipped own moves). returns false when nothing new remains.
// call it in a loop each time a gameState arrives to drain new opponent moves.
bool lichess_next_opponent_move(const char *moves, int my_color,
                                int *applied, char *out);

// feed one received byte into a line buffer. returns true when a full line has
// been assembled (on '\n'); the line (newline stripped, '\r' ignored) is left
// NUL-terminated in buf and *len is reset to 0. used to read NDJSON streams one
// line at a time without blocking.
bool ndjson_feed(char *buf, int cap, int *len, char c);

// resolve our color from a gameFull's white/black player ids and our username.
// case-insensitive. writes 0 (white) or 1 (black) to *out, returns false if
// neither id matches us.
bool lichess_my_color(const char *white_id, const char *black_id,
                      const char *me, int *out);

#ifdef __cplusplus
}
#endif

#endif // CHESSLINK_LICHESS_H
