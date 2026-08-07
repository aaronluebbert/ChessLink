#include "chesslink.h"
#include "chess_engine.h"   // rebuild the authoritative position from the move list
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <DNSServer.h>

static const char *LICHESS_START_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// --- config ------------------------------------------------------------------
// WiFi SSID/password and the lichess token all live in NVS namespace "wifi"
// (keys "ssid" / "pass" / "token"), written by the on-device captive portal
// below. nothing is hardcoded.

#include <Preferences.h>
static Preferences prefs;

#define LICHESS_BASE   "https://lichess.org"

// lichess API token -- loaded from NVS at boot, never hardcoded so nothing
// secret is committed. the setup protocol stores it in NVS namespace "wifi",
// key "token" (same namespace as the WiFi credentials)
static char lichess_token[96] = {};

static void load_lichess_token(void) {
    prefs.begin("wifi", true);
    String tok = prefs.getString("token", "");
    prefs.end();
    strncpy(lichess_token, tok.c_str(), sizeof(lichess_token) - 1);
    lichess_token[sizeof(lichess_token) - 1] = '\0';
    if (lichess_token[0] == '\0')
        Serial.println("[net] no lichess token in NVS -- run setup first");
}

// add the bearer auth header using the runtime token
static void add_auth(HTTPClient &http) {
    char auth[112];
    snprintf(auth, sizeof(auth), "Bearer %s", lichess_token);
    http.addHeader("Authorization", auth);
}

static bool wifi_connect(void);   // defined below

// report coarse status up to the game/display task
static void net_report(NetStatus_t s) {
    NetUpdate_t up = {};
    up.has_status = true;
    up.status     = s;
    xQueueSend(xQ_NetUpdate, &up, 0);
}

// report a finished-game result (shown on the game-over screen)
static void net_report_result(const char *text) {
    NetUpdate_t up = {};
    up.has_result = true;
    strncpy(up.result_text, text, sizeof(up.result_text) - 1);
    xQueueSend(xQ_NetUpdate, &up, 0);
}

// set when a terminal gameState arrives, so the stream loop can break cleanly
static volatile bool s_game_ended = false;

// --- captive-portal setup ----------------------------------------------------
//
// on first boot (or on demand from the menu) we bring up a soft-AP and a small
// web form so the user can enter their WiFi SSID/password and lichess token
// from a phone. a DNS wildcard makes the setup page pop up automatically. the
// values are written to NVS ("wifi" namespace) and we then join the network.

static WebServer     setup_web(80);
static DNSServer     setup_dns;
static volatile bool setup_submitted = false;

static const char SETUP_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>ChessLink setup</title><style>body{font-family:sans-serif;margin:22px;max-width:420px}
label{font-size:14px;color:#333}input{width:100%;padding:10px;margin:6px 0 14px;font-size:16px;box-sizing:border-box}
button{width:100%;padding:12px;font-size:16px;border:0;background:#2d6cdf;color:#fff;border-radius:6px}</style>
</head><body><h2>ChessLink setup</h2>
<form method=POST action=/save>
<label>WiFi network</label><input name=ssid required>
<label>WiFi password</label><input name=pass type=password>
<label>Lichess API token</label><input name=token>
<button type=submit>Save &amp; connect</button></form>
<p style="color:#666;font-size:13px">Token: lichess.org &gt; Preferences &gt; API access tokens, board:play scope.</p>
</body></html>
)HTML";

static void setup_handle_root() { setup_web.send_P(200, "text/html", SETUP_PAGE); }

static void setup_handle_save() {
    prefs.begin("wifi", false);   // read-write
    prefs.putString("ssid",  setup_web.arg("ssid"));
    prefs.putString("pass",  setup_web.arg("pass"));
    prefs.putString("token", setup_web.arg("token"));
    prefs.end();

    setup_web.send(200, "text/html",
        "<html><body style='font-family:sans-serif;margin:22px'><h2>Saved</h2>"
        "<p>ChessLink is connecting to your WiFi. You can close this page.</p></body></html>");

    setup_submitted = true;
}

// bring up the AP + portal and block until the phone submits the form
static void run_setup_portal(void) {
    setup_submitted = false;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SETUP_AP_SSID, WIFI_SETUP_AP_PASS);
    IPAddress ip = WiFi.softAPIP();   // 192.168.4.1
    Serial.printf("[net] setup AP '%s' up -> http://%s\n",
                  WIFI_SETUP_AP_SSID, ip.toString().c_str());

    setup_dns.start(53, "*", ip);     // resolve every host to us (captive portal)
    setup_web.on("/",     setup_handle_root);
    setup_web.on("/save", HTTP_POST, setup_handle_save);
    setup_web.onNotFound(setup_handle_root);   // any probe URL shows the form
    setup_web.begin();

    net_report(NET_STATUS_SETUP);     // tell the display to show instructions

    while (!setup_submitted) {
        setup_dns.processNextRequest();
        setup_web.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    setup_web.stop();
    setup_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("[net] setup submitted, credentials saved");
}

// make sure we're on WiFi: try saved creds, run the portal if that fails, then
// reload the token and report ready. loops until connected
static void ensure_connected(void) {
    for (;;) {
        if (wifi_connect()) {
            load_lichess_token();     // portal may have just written a new one
            net_report(NET_STATUS_ONLINE);
            return;
        }
        run_setup_portal();           // no creds or connect failed -- provision
    }
}

// --- internal state ----------------------------------------------------------

typedef enum {
    NET_STATE_DISCONNECTED,
    NET_STATE_IDLE,
    NET_STATE_IN_GAME,
} NetState_t;

static NetState_t net_state      = NET_STATE_DISCONNECTED;
static char       game_id[24]    = {};   // lichess game IDs are 8 chars but give headroom
static bool       we_are_white   = true; // set from gameFull, determines which moves are ours

// clock state (absolute remaining ms; the server value already includes any
// increment, so we don't track increments separately)
static uint32_t s_white_clock_ms = 0;
static uint32_t s_black_clock_ms = 0;

// seek parameters chosen in the online config submenu
static uint16_t s_seek_time_min  = 5;
static uint8_t  s_seek_inc_sec   = 3;
static bool     s_seek_rated     = true;

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
    add_auth(http);
    http.addHeader("Content-Length", "0");

    int code = http.POST("");
    if (code != 200) Serial.printf("[net] post move failed: HTTP %d\n", code);
    http.end();
    return code == 200;
}

// POST a simple board-game action with no body (resign / abort)
static bool lichess_game_action(const char *gid, const char *action) {
    if (WiFi.status() != WL_CONNECTED) return false;
    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/%s/%s", LICHESS_BASE, gid, action);
    HTTPClient http;
    http.begin(url);
    add_auth(http);
    http.addHeader("Content-Length", "0");
    int code = http.POST("");
    http.end();
    return code == 200;
}

// resign the game; fall back to abort if it's too early in the game to resign
static bool lichess_resign(const char *gid) {
    if (lichess_game_action(gid, "resign")) return true;
    return lichess_game_action(gid, "abort");
}

// --- move color tracking -----------------------------------------------------
//
// lichess streams the FULL moves list on every gameState event. Rather than
// track moves incrementally (which drifts if a packet is missed or a move is
// rejected), we rebuild the whole position from that list every time and hand it
// to the game task as an authoritative truth-sync. The board always defers to it.

// --- stream line handler -----------------------------------------------------

static void handle_stream_line(const char *line) {
    // gameFull carries both player objects plus the initial state, so give the
    // parser headroom -- gameState lines are much smaller
    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, line)) {
        Serial.println("[net] JSON parse error");
        return;
    }

    const char *type = doc["type"];
    if (!type) return;

    if (strcmp(type, "gameFull") == 0) {
        // gameFull arrives first on the stream -- game metadata + players
        // our color is normally already known from the gameStart event; fall
        // back to the orientation field if the stream provides it
        const char *orientation = doc["orientation"];
        if (orientation) we_are_white = (strcmp(orientation, "white") == 0);

        // player names + ratings for both sides (bots/anon may lack fields)
        JsonObjectConst white = doc["white"].as<JsonObjectConst>();
        JsonObjectConst black = doc["black"].as<JsonObjectConst>();
        const char *w_name = white["name"] | white["id"] | "White";
        const char *b_name = black["name"] | black["id"] | "Black";
        uint16_t    w_rate = white["rating"] | 0;
        uint16_t    b_rate = black["rating"] | 0;
        if (white.containsKey("aiLevel")) w_name = "Stockfish";  // bot game
        if (black.containsKey("aiLevel")) b_name = "Stockfish";

        // initial clocks from the nested state
        JsonObjectConst state = doc["state"].as<JsonObjectConst>();
        if (!state.isNull()) {
            s_white_clock_ms = state["wtime"] | s_white_clock_ms;
            s_black_clock_ms = state["btime"] | s_black_clock_ms;
        }

        Serial.printf("[net] we are %s -- %s (%u) vs %s (%u)\n",
                      we_are_white ? "white" : "black",
                      w_name, w_rate, b_name, b_rate);

        // push identities (resolved to me/opp) + initial clocks to the game task
        NetUpdate_t up = {};
        up.has_meta  = true;
        up.my_color  = we_are_white ? 0 : 1;
        strncpy(up.my_name,  we_are_white ? w_name : b_name, sizeof(up.my_name)  - 1);
        strncpy(up.opp_name, we_are_white ? b_name : w_name, sizeof(up.opp_name) - 1);
        up.my_rating  = we_are_white ? w_rate : b_rate;
        up.opp_rating = we_are_white ? b_rate : w_rate;

        up.has_clocks     = true;
        up.white_clock_ms = s_white_clock_ms;
        up.black_clock_ms = s_black_clock_ms;

        xQueueSend(xQ_NetUpdate, &up, 0);
        return;
    }

    if (strcmp(type, "gameState") == 0) {
        // game finished? a terminal status ends the stream and shows a result.
        // (the final move isn't guided onto the board -- the game is already over)
        const char *status = doc["status"];
        if (status && strcmp(status, "started") != 0 && strcmp(status, "created") != 0) {
            const char *winner = doc["winner"];   // "white"/"black", null on a draw
            char text[32];
            if (strcmp(status, "aborted") == 0) {
                snprintf(text, sizeof(text), "Game aborted");
            } else if (!winner) {
                snprintf(text, sizeof(text), "Draw: %s", status);
            } else {
                bool we_won = ((strcmp(winner, "white") == 0) == we_are_white);
                snprintf(text, sizeof(text), "%s: %s",
                         we_won ? "You won" : "You lost", status);
            }
            Serial.printf("[net] game over: %s\n", text);
            net_report_result(text);
            s_game_ended = true;
            return;
        }

        // clock update
        if (doc.containsKey("wtime")) s_white_clock_ms = doc["wtime"].as<uint32_t>();
        if (doc.containsKey("btime")) s_black_clock_ms = doc["btime"].as<uint32_t>();

        Serial.printf("[net] clock -- w:%lums b:%lums\n", s_white_clock_ms, s_black_clock_ms);

        // Rebuild the authoritative position from the server's full move list and
        // push it to the game task as a truth-sync (works for our own moves and
        // the opponent's alike -- the board just adopts whatever the server says).
        const char *moves = doc["moves"] | "";

        Position sp;
        pos_from_fen(&sp, LICHESS_START_FEN);
        uint8_t last_from = 0, last_to = 0;
        bool    have_last = false;
        if (strlen(moves)) {
            char buf[640];
            strncpy(buf, moves, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *save = nullptr;
            for (char *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(nullptr, " ", &save)) {
                if (strlen(tok) < 4) continue;
                Move m = uci_to_move(&sp, tok);
                if (m == MOVE_NONE) break;          // stop replaying rather than corrupt
                Position undo;
                make_move_pos(&sp, m, &undo);
                last_from = (uint8_t)((tok[0] - 'a') + (tok[1] - '1') * 8);
                last_to   = (uint8_t)((tok[2] - 'a') + (tok[3] - '1') * 8);
                have_last = true;
            }
        }

        NetUpdate_t up = {};
        up.has_sync = true;
        pos_to_fen(&sp, up.sync_fen, sizeof(up.sync_fen));
        up.sync_have_last = have_last;
        up.sync_from      = last_from;
        up.sync_to        = last_to;
        up.has_clocks     = true;
        up.white_clock_ms = s_white_clock_ms;
        up.black_clock_ms = s_black_clock_ms;
        xQueueSend(xQ_NetUpdate, &up, 0);
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
    add_auth(http);
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

    s_game_ended = false;   // cleared per game, set by a terminal gameState
    Serial.printf("[net] streaming game %s\n", gid);

    while (WiFi.status() == WL_CONNECTED && !s_game_ended) {
        // a leave/resign request from the game task (read here since the top task
        // loop is parked inside this stream for the duration of the game)
        NetCmdMsg_t nc;
        if (xQueueReceive(xQ_NetCmd, &nc, 0) == pdTRUE && nc.type == NET_CMD_RESIGN) {
            lichess_resign(gid);
            net_report_result("You resigned");
            s_game_ended = true;
            break;
        }

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
    add_auth(http);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(120000);  // seek can take a while depending on pool size

    Serial.println("[net] posting seek...");

    // POST returns a stream -- we read it for the gameStart event
    char body[80];
    snprintf(body, sizeof(body), "rated=%s&time=%u&increment=%u&variant=standard",
             s_seek_rated ? "true" : "false",
             (unsigned)s_seek_time_min, (unsigned)s_seek_inc_sec);
    int code = http.POST(body);
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

                StaticJsonDocument<512> doc;
                if (deserializeJson(doc, line_buf)) continue;

                const char *type = doc["type"];
                if (!type) continue;

                if (strcmp(type, "gameStart") == 0) {
                    // gameStart: {"type":"gameStart","game":{"id":"...","color":"white",...}}
                    // the color field is our side -- authoritative, use it
                    const char *color = doc["game"]["color"];
                    if (color) we_are_white = (strcmp(color, "white") == 0);

                    const char *gid = doc["game"]["id"];
                    if (gid && strlen(gid) > 0) {
                        strncpy(out_id, gid, id_len - 1);
                        out_id[id_len - 1] = '\0';
                        Serial.printf("[net] game found: %s (we are %s)\n",
                                      out_id, we_are_white ? "white" : "black");
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

// --- challenge Stockfish -----------------------------------------------------
//
// POST /api/challenge/ai returns the created game immediately (no seek). we read
// the game id and, for a random color, our assigned side, then stream it like
// any other game.

static bool lichess_challenge_ai(char *out_id, size_t id_len, const NetCmdMsg_t *cmd) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(LICHESS_BASE "/api/challenge/ai");
    add_auth(http);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    const char *col = cmd->color == 0 ? "white" : cmd->color == 1 ? "black" : "random";
    char body[128];
    if (cmd->time_min > 0)   // real-time: lichess wants clock.limit in seconds
        snprintf(body, sizeof(body),
                 "level=%u&color=%s&variant=standard&clock.limit=%u&clock.increment=%u",
                 (unsigned)cmd->level, col,
                 (unsigned)(cmd->time_min * 60), (unsigned)cmd->inc_sec);
    else                     // untimed
        snprintf(body, sizeof(body), "level=%u&color=%s&variant=standard",
                 (unsigned)cmd->level, col);

    Serial.printf("[net] challenge ai: %s\n", body);
    int code = http.POST(body);
    if (code != 200 && code != 201) {
        Serial.printf("[net] challenge ai failed: HTTP %d\n", code);
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, payload)) return false;

    const char *gid = doc["id"];
    if (!gid || strlen(gid) == 0) return false;
    strncpy(out_id, gid, id_len - 1);
    out_id[id_len - 1] = '\0';

    // explicit color requests are certain; for random, read it back (gameFull
    // orientation is the fallback if the response omits it)
    if (cmd->color == 0)      we_are_white = true;
    else if (cmd->color == 1) we_are_white = false;
    else {
        const char *c = doc["color"];
        if (!c) c = doc["player"];
        if (c) we_are_white = (strcmp(c, "white") == 0);
    }
    Serial.printf("[net] bot game %s, we are %s\n", out_id, we_are_white ? "white" : "black");
    return true;
}

// --- task --------------------------------------------------------------------

void task_Network(void *pvParameters) {
    NetCmdMsg_t cmd;

    // connect on boot, running the captive portal first if there are no creds
    ensure_connected();
    net_state = NET_STATE_IDLE;

    for (;;) {
        if (xQueueReceive(xQ_NetCmd, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {

            // seek a game and play it (player moves are drained and posted
            // inside lichess_stream_game). errors surface on the LCD via a
            // result notice that returns to the menu
            if (cmd.type == NET_CMD_START_ONLINE && net_state == NET_STATE_IDLE) {
                s_seek_time_min = cmd.time_min;
                s_seek_inc_sec  = cmd.inc_sec;
                s_seek_rated    = cmd.rated;
                if (lichess_token[0] == '\0') {
                    net_report_result("No token: run setup");
                } else {
                    memset(game_id, 0, sizeof(game_id));
                    if (lichess_seek_and_get_id(game_id, sizeof(game_id))) {
                        net_state = NET_STATE_IN_GAME;
                        lichess_stream_game(game_id);
                        if (!s_game_ended) net_report_result("Connection lost");
                        net_state = NET_STATE_IDLE;
                        memset(game_id, 0, sizeof(game_id));
                    } else {
                        net_report_result("Seek failed");
                    }
                }

            // reopen the captive portal to change WiFi / token, then reconnect
            // challenge Stockfish and play it
            } else if (cmd.type == NET_CMD_START_BOT && net_state == NET_STATE_IDLE) {
                if (lichess_token[0] == '\0') {
                    net_report_result("No token: run setup");
                } else {
                    memset(game_id, 0, sizeof(game_id));
                    if (lichess_challenge_ai(game_id, sizeof(game_id), &cmd)) {
                        net_state = NET_STATE_IN_GAME;
                        lichess_stream_game(game_id);
                        if (!s_game_ended) net_report_result("Connection lost");
                        net_state = NET_STATE_IDLE;
                        memset(game_id, 0, sizeof(game_id));
                    } else {
                        net_report_result("Bot start failed");
                    }
                }

            } else if (cmd.type == NET_CMD_OPEN_SETUP) {
                run_setup_portal();                 // shows setup screen, user submits
                net_report(NET_STATUS_CONNECTING);  // feedback while we join WiFi
                ensure_connected();                 // -> ONLINE (menu), or reopens the portal
                net_state = NET_STATE_IDLE;
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            net_state = NET_STATE_DISCONNECTED;
            Serial.println("[net] connection dropped, reconnecting...");
            while (!wifi_connect())
                vTaskDelay(pdMS_TO_TICKS(10000));  // transient drop -- just retry
            net_report(NET_STATUS_ONLINE);
            net_state = NET_STATE_IDLE;
        }
    }

    vTaskDelete(NULL);
}
