// lichess.cpp -- pure parsing helpers, host-testable

#include "lichess.h"
#include <string.h>
#include <ctype.h>

int lichess_move_count(const char *moves) {
    if (!moves) return 0;
    int n = 0;
    const char *p = moves;
    while (*p) {
        while (*p == ' ') p++;          // skip separators
        if (!*p) break;
        n++;
        while (*p && *p != ' ') p++;    // skip the token
    }
    return n;
}

bool lichess_nth_move(const char *moves, int idx, char *out) {
    if (!moves || idx < 0) return false;
    int n = 0;
    const char *p = moves;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if (n == idx) {
            int len = (int)(p - start);
            if (len < 4 || len > 5) return false;
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        n++;
    }
    return false;
}

bool lichess_next_opponent_move(const char *moves, int my_color,
                                int *applied, char *out) {
    int total = lichess_move_count(moves);
    int opp = my_color ^ 1;
    while (*applied < total) {
        int idx = *applied;
        (*applied)++;
        if (lichess_ply_color(idx) == opp)
            return lichess_nth_move(moves, idx, out);   // next opponent ply
        // else it was our own move, already on the board -- skip it
    }
    return false;
}

bool ndjson_feed(char *buf, int cap, int *len, char c) {
    if (c == '\r') return false;         // ignore CR
    if (c == '\n') { buf[*len] = '\0'; *len = 0; return true; }
    if (*len < cap - 1) buf[(*len)++] = c;  // silently drop overlong lines
    return false;
}

static bool ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

bool lichess_my_color(const char *white_id, const char *black_id,
                      const char *me, int *out) {
    if (ieq(white_id, me)) { *out = 0; return true; }
    if (ieq(black_id, me)) { *out = 1; return true; }
    return false;
}
