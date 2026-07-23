// famous_games.h -- a small table of famous games for the Local > Famous menu
//
// each game is a start position (0 = standard start) plus a list of uci moves.
// the game task replays one move-by-move, guiding the player through it with the
// board LEDs (reusing the same sync-guidance used for opponent moves).

#ifndef CHESSLINK_FAMOUS_GAMES_H
#define CHESSLINK_FAMOUS_GAMES_H

#ifdef __cplusplus
extern "C" {
#endif

#define FG_MAX_PLIES 64

typedef struct {
    const char *name;                 // short label for the menu / status line
    const char *start_fen;            // 0 = standard start position
    const char *moves[FG_MAX_PLIES];  // uci moves, terminated by a 0 entry
    int         ply_count;
    bool        ends_mate;            // true if the last move is checkmate
} FamousGame;

extern const FamousGame FAMOUS_GAMES[];
extern const int FAMOUS_GAME_COUNT;

#ifdef __cplusplus
}
#endif

#endif // CHESSLINK_FAMOUS_GAMES_H
