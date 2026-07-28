#include "chesslink.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// --- config ------------------------------------------------------------------
// WiFi credentials: loaded from NVS by groupmate's provisioning code.
// these are fallback defines only -- if NVS has credentials, those win.
// see task_NetSetup (groupmate's code) which writes "ssid"/"pass" to NVS
// under namespace "wifi" before this task runs.

#include <Preferences.h>
static Preferences prefs;

#define LICHESS_TOKEN  "YOUR_LICHESS_API_TOKEN"
#define LICHESS_BASE   "https://lichess.org"

// --- internal state ----------------------------------------------------------

typedef enum {
    NET_STATE_DISCONNECTED,
    NET_STATE_IDLE,
    NET_STATE_IN_GAME,
} NetState_t;

static NetState_t net_state      = NET_STATE_DISCONNECTED;
static char       game_id[24]    = {};   // lichess game IDs are 8 chars but give headroom
static bool       we_are_white   = true; // set from gameFull, determines which moves are ours

// clock state
static uint32_t s_white_clock_ms = 0;
static uint32_t s_black_clock_ms = 0;
static uint32_t s_white_inc_ms   = 0;
static uint32_t s_black_inc_ms   = 0;

// --- WiFi --------------------------------------------------------------------
//
// reads SSID/pass from NVS namespace "wifi", keys "ssid" and "pass".
// this is the namespace groupmate's captive portal writes to.
// if NVS is empty (first boot, or never provisioned) returns false.

static bool wifi_connect(void) {
    prefs.begin("wifi", true);  // read-only
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();

    if (ssid.length() == 0) {
        Serial.println("[net] no WiFi credentials in NVS -- run provisioning first");
        return false;
    }

    Serial.printf("[net] connecting to %s...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());

    for (int elapsed = 0; elapsed < 15000 && WiFi.status() != WL_CONNECTED; elapsed += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[net] connected -- IP %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("\n[net] WiFi connect failed");
    return false;
}

// --- lichess helpers ---------------------------------------------------------

// POST /api/board/game/{gameId}/move/{uci}
static bool lichess_post_move(const char *gid, const char *uci) {
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/%s/move/%s", LICHESS_BASE, gid, uci);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Length", "0");

    int code = http.POST("");
    if (code != 200) Serial.printf("[net] post move failed: HTTP %d\n", code);
    http.end();
    return code == 200;
}

// --- move color tracking -----------------------------------------------------
//
// lichess streams the full moves list on every gameState event.
// we count how many moves have been played -- even count means white just moved,
// odd means black just moved. combined with we_are_white we know if it's ours.
//
// we only forward moves to game logic when it's the OPPONENT who just moved.
// this prevents the board from trying to re-apply our own move.

static int count_moves(const char *moves_str) {
    if (!moves_str || !strlen(moves_str)) return 0;
    int count = 1;
    for (const char *p = moves_str; *p; p++)
        if (*p == ' ') count++;
    return count;
}

// --- stream line handler -----------------------------------------------------

static void handle_stream_line(const char *line) {
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, line)) {
        Serial.println("[net] JSON parse error");
        return;
    }

    const char *type = doc["type"];
    if (!type) return;

    if (strcmp(type, "gameFull") == 0) {
        // gameFull arrives first on the stream -- extract game metadata
        // figure out which color we are by matching token owner's username
        // lichess puts our color under white.id or black.id
        // TODO: when Lichess OAuth is integrated, compare against the authed username.
        // for now we read "orientation" which lichess sets to our color.
        const char *orientation = doc["orientation"];
        if (orientation) {
            we_are_white = (strcmp(orientation, "white") == 0);
            Serial.printf("[net] we are %s\n", we_are_white ? "white" : "black");
        }

        // process the nested initial state
        JsonObjectConst state = doc["state"].as<JsonObjectConst>();
        if (state.isNull()) return;

        // clock from initial state
        if (state.containsKey("wtime")) s_white_clock_ms = state["wtime"].as<uint32_t>();
        if (state.containsKey("btime")) s_black_clock_ms = state["btime"].as<uint32_t>();
        if (state.containsKey("winc"))  s_white_inc_ms   = state["winc"].as<uint32_t>();
        if (state.containsKey("binc"))  s_black_inc_ms   = state["binc"].as<uint32_t>();

        // no moves to forward on gameFull -- game is just starting
        return;
    }

    if (strcmp(type, "gameState") == 0) {
        // clock update
        if (doc.containsKey("wtime")) s_white_clock_ms = doc["wtime"].as<uint32_t>();
        if (doc.containsKey("btime")) s_black_clock_ms = doc["btime"].as<uint32_t>();
        if (doc.containsKey("winc"))  s_white_inc_ms   = doc["winc"].as<uint32_t>();
        if (doc.containsKey("binc"))  s_black_inc_ms   = doc["binc"].as<uint32_t>();

        Serial.printf("[net] clock -- w:%lums b:%lums\n", s_white_clock_ms, s_black_clock_ms);

        const char *moves = doc["moves"];
        if (!moves || !strlen(moves)) return;

        int n = count_moves(moves);
        // after n moves: if n is odd, white just moved; if even, black just moved
        // (move 1 = white, move 2 = black, ...)
        bool white_just_moved = (n % 2 == 1);
        bool opponent_just_moved = (we_are_white) ? !white_just_moved : white_just_moved;

        if (!opponent_just_moved) return;  // our own move echoed back, ignore it

        // extract the last UCI token
        const char *last = strrchr(moves, ' ');
        const char *uci  = last ? last + 1 : moves;
        if (strlen(uci) < 4) return;

        MoveEvent_t opp = { .src = MOVE_SRC_OPPONENT };
        strncpy(opp.uci, uci, sizeof(opp.uci) - 1);
        opp.from_sq        = (uint8_t)((opp.uci[0] - 'a') + (opp.uci[1] - '1') * 8);
        opp.to_sq          = (uint8_t)((opp.uci[2] - 'a') + (opp.uci[3] - '1') * 8);
        opp.white_clock_ms = s_white_clock_ms;
        opp.black_clock_ms = s_black_clock_ms;
        opp.white_inc_ms   = s_white_inc_ms;
        opp.black_inc_ms   = s_black_inc_ms;
        xQueueSend(xQ_OpponentMove, &opp, 0);
        return;
    }

    if (strcmp(type, "gameFinish") == 0) {
        Serial.println("[net] game finished");
        // game logic will notice the board is back at starting position or similar
        // for now just log it -- could send a special event type later
    }
}

// --- game stream -------------------------------------------------------------
//
// GET /api/board/game/stream/{gameId}
// blocks until game ends or connection drops

static void lichess_stream_game(const char *gid) {
    if (WiFi.status() != WL_CONNECTED) return;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/stream/%s", LICHESS_BASE, gid);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.setTimeout(60000);  // lichess sends keep-alive newlines every ~10s

    if (http.GET() != 200) {
        Serial.printf("[net] stream open failed\n");
        http.end();
        return;
    }

    WiFiClient *stream = http.getStreamPtr();
    char line_buf[512];
    int  line_len = 0;

    MoveEvent_t      player_mv;
    TickType_t       last_data    = xTaskGetTickCount();
    const TickType_t STREAM_TIMEOUT = pdMS_TO_TICKS(60000);  // generous, lichess keep-alives every ~10s

    Serial.printf("[net] streaming game %s\n", gid);

    while (WiFi.status() == WL_CONNECTED) {
        // flush any player moves while reading
        if (xQueueReceive(xQ_PlayerMove, &player_mv, 0) == pdTRUE)
            lichess_post_move(gid, player_mv.uci);

        if (stream->available()) {
            char c = (char)stream->read();
            last_data = xTaskGetTickCount();

            if (c == '\n') {
                line_buf[line_len] = '\0';
                line_len = 0;
                if (strlen(line_buf) > 0)
                    handle_stream_line(line_buf);
            } else if (line_len < (int)sizeof(line_buf) - 2) {
                line_buf[line_len++] = c;
            }

        } else {
            if ((xTaskGetTickCount() - last_data) > STREAM_TIMEOUT) {
                Serial.println("[net] stream timeout");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    http.end();
    Serial.println("[net] stream closed");
}

// --- seek game ---------------------------------------------------------------
//
// POST /api/board/seek is itself a streaming endpoint on lichess.
// it holds the connection open and returns a "gameStart" event when a match
// is found, containing the game ID we need.
// we stream it just like the game stream until we get that event.

static bool lichess_seek_and_get_id(char *out_id, size_t id_len) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(LICHESS_BASE "/api/board/seek");
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(120000);  // seek can take a while depending on pool size

    Serial.println("[net] posting seek...");

    // POST returns a stream -- we read it for the gameStart event
    int code = http.POST("rated=true&time=5&increment=3&variant=standard");
    if (code != 200) {
        Serial.printf("[net] seek failed: HTTP %d\n", code);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    char line_buf[256];
    int  line_len = 0;
    bool found    = false;

    TickType_t       start         = xTaskGetTickCount();
    const TickType_t SEEK_TIMEOUT  = pdMS_TO_TICKS(120000);

    while (WiFi.status() == WL_CONNECTED
           && (xTaskGetTickCount() - start) < SEEK_TIMEOUT) {

        if (stream->available()) {
            char c = (char)stream->read();

            if (c == '\n') {
                line_buf[line_len] = '\0';
                line_len = 0;

                if (strlen(line_buf) == 0) continue;

                StaticJsonDocument<256> doc;
                if (deserializeJson(doc, line_buf)) continue;

                const char *type = doc["type"];
                if (!type) continue;

                if (strcmp(type, "gameStart") == 0) {
                    // gameStart event: {"type":"gameStart","game":{"id":"abc12345",...}}
                    const char *gid = doc["game"]["id"];
                    if (gid && strlen(gid) > 0) {
                        strncpy(out_id, gid, id_len - 1);
                        out_id[id_len - 1] = '\0';
                        Serial.printf("[net] game found: %s\n", out_id);
                        found = true;
                        break;
                    }
                }

            } else if (line_len < (int)sizeof(line_buf) - 2) {
                line_buf[line_len++] = c;
            }

        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    http.end();
    return found;
}

// --- task --------------------------------------------------------------------

void task_Network(void *pvParameters) {
    MoveEvent_t player_mv;

    // retry WiFi indefinitely -- provisioning must run first
    while (!wifi_connect())
        vTaskDelay(pdMS_TO_TICKS(10000));  // retry every 10s, not 30s, feels more responsive

    net_state = NET_STATE_IDLE;

    for (;;) {
        // wait for a player move as the trigger to seek or post
        if (xQueueReceive(xQ_PlayerMove, &player_mv, pdMS_TO_TICKS(5000)) == pdTRUE) {

            if (net_state == NET_STATE_IN_GAME) {
                lichess_post_move(game_id, player_mv.uci);

            } else if (net_state == NET_STATE_IDLE) {
                // seek a game and get the real ID before streaming
                memset(game_id, 0, sizeof(game_id));
                if (lichess_seek_and_get_id(game_id, sizeof(game_id))) {
                    net_state = NET_STATE_IN_GAME;
                    lichess_stream_game(game_id);
                    net_state = NET_STATE_IDLE;
                    memset(game_id, 0, sizeof(game_id));
                } else {
                    Serial.println("[net] seek timed out or failed");
                }
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            net_state = NET_STATE_DISCONNECTED;
            Serial.println("[net] connection dropped, reconnecting...");
            while (!wifi_connect())
                vTaskDelay(pdMS_TO_TICKS(10000));
            net_state = NET_STATE_IDLE;
        }
    }

    vTaskDelete(NULL);
}
