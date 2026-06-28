#include "chesslink.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// config (move to NVS in production)

#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASS      "YOUR_PASS"
#define LICHESS_TOKEN  "YOUR_LICHESS_API_TOKEN"
#define LICHESS_BASE   "https://lichess.org"

// internal state

typedef enum {
    NET_STATE_DISCONNECTED,
    NET_STATE_IDLE,
    NET_STATE_IN_GAME,
} NetState_t;

static NetState_t net_state   = NET_STATE_DISCONNECTED;
static char       game_id[16] = {};

// WiFi

static bool wifi_connect(void) {
    Serial.printf("[net] connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    for (int elapsed = 0; elapsed < 15000 && WiFi.status() != WL_CONNECTED; elapsed += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[net] connected -- IP %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("\n[net] WiFi failed");
    return false;
}

// lichess API helpers

// POST /api/board/game/{gameId}/move/{uci}
static bool lichess_post_move(const char *game, const char *uci) {
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/%s/move/%s", LICHESS_BASE, game, uci);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Length", "0");

    int code = http.POST("");
    if (code != 200) Serial.printf("[net] post move failed: HTTP %d\n", code);
    http.end();
    return code == 200;
}

// POST /api/board/seek -- 5+3 blitz, rated
static bool lichess_seek_game(void) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(LICHESS_BASE "/api/board/seek");
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST("rated=true&time=5&increment=3&variant=standard");
    Serial.printf("[net] seek result: HTTP %d\n", code);
    http.end();
    return code == 200 || code == 201;
}

// GET /api/board/game/stream/{gameId} -- NDJSON long-poll.
// reads until connection drops or a player move arrives to post.
// TODO: replace with proper streaming HTTP client for robustness
static void lichess_stream_game(const char *game) {
    if (WiFi.status() != WL_CONNECTED) return;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/stream/%s", LICHESS_BASE, game);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.setTimeout(30000);

    if (http.GET() != 200) { http.end(); return; }

    WiFiClient *stream = http.getStreamPtr();
    char line_buf[256];
    int  line_len = 0;

    MoveEvent_t  player_mv;
    TickType_t   last_data = xTaskGetTickCount();
    const TickType_t STREAM_TIMEOUT = pdMS_TO_TICKS(30000);

    while (WiFi.status() == WL_CONNECTED) {
        // post player moves without breaking the read loop
        if (xQueueReceive(xQ_PlayerMove, &player_mv, 0) == pdTRUE)
            lichess_post_move(game, player_mv.uci);

        if (stream->available()) {
            char c = (char)stream->read();
            last_data = xTaskGetTickCount();

            if (c == '\n') {
                line_buf[line_len] = '\0';
                line_len = 0;

                if (strlen(line_buf) == 0) continue;  // keep-alive newline

                StaticJsonDocument<512> doc;
                if (deserializeJson(doc, line_buf)) continue;

                const char *type = doc["type"];
                if (!type) continue;

                if (strcmp(type, "gameState") == 0) {
                    const char *moves = doc["moves"];
                    if (!moves || !strlen(moves)) continue;

                    // last space-delimited token is the most recent move
                    const char *last = strrchr(moves, ' ');
                    const char *uci  = last ? last + 1 : moves;

                    if (strlen(uci) < 4) continue;

                    // TODO: track color assignment properly, this blindly forwards
                    // every state update as an opponent move
                    MoveEvent_t opp = { .src = MOVE_SRC_OPPONENT };
                    strncpy(opp.uci, uci, sizeof(opp.uci) - 1);
                    opp.from_sq = (uint8_t)((opp.uci[0] - 'a') + (opp.uci[1] - '1') * 8);
                    opp.to_sq   = (uint8_t)((opp.uci[2] - 'a') + (opp.uci[3] - '1') * 8);
                    xQueueSend(xQ_OpponentMove, &opp, 0);
                }

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
}

// task

void task_Network(void *pvParameters) {
    MoveEvent_t player_mv;

    while (!wifi_connect())
        vTaskDelay(pdMS_TO_TICKS(30000));
    net_state = NET_STATE_IDLE;

    for (;;) {
        if (xQueueReceive(xQ_PlayerMove, &player_mv, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (net_state == NET_STATE_IN_GAME) {
                lichess_post_move(game_id, player_mv.uci);

            } else if (net_state == NET_STATE_IDLE) {
                Serial.println("[net] seeking lichess game...");
                if (lichess_seek_game()) {
                    // TODO: extract real game_id from seek response
                    strncpy(game_id, "stubgameid1", sizeof(game_id) - 1);
                    net_state = NET_STATE_IN_GAME;
                    lichess_stream_game(game_id);
                    net_state = NET_STATE_IDLE;
                }
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            net_state = NET_STATE_DISCONNECTED;
            Serial.println("[net] dropped, reconnecting...");
            wifi_connect();
            net_state = NET_STATE_IDLE;
        }
    }

    vTaskDelete(NULL);
}
