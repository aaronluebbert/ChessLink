#include "chesslink.h"

// input task -- the only owner of Serial and the four buttons (PCB v3). each
// button maps straight to a UI event; serial keys mirror them for bench use.
// buttons are input-only pins with external pull-ups, active low (pressed = LOW).
// every event also carries the raw typed line in ev.text (unused now that setup
// is the phone portal, but harmless and handy for debugging).

// --- serial line assembly (non-blocking) ------------------------------------

static bool read_line(String &out) {
    static String buf = "";
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (buf.length() > 0) { buf.trim(); out = buf; buf = ""; return true; }
        } else if (buf.length() < 63) {
            buf += c;
        }
    }
    return false;
}

static void post(UiEventType_t type, const char *text) {
    UiEvent_t ev;
    ev.type = type;
    ev.text[0] = '\0';
    if (text) { strncpy(ev.text, text, sizeof(ev.text) - 1); ev.text[sizeof(ev.text) - 1] = '\0'; }
    xQueueSend(xQ_UiEvent, &ev, 0);
}

// classify a typed serial line, keeping the raw text attached
static void handle_line(const String &line) {
    String c = line; c.toLowerCase();
    const char *raw = line.c_str();
    if      (c == "w")     post(UI_EV_UP,        raw);
    else if (c == "s")     post(UI_EV_DOWN,      raw);
    else if (c == "e")     post(UI_EV_SELECT,    raw);
    else if (c == "q")     post(UI_EV_BACK,      raw);
    else if (c == "wifi")  post(UI_EV_CMD_WIFI,  raw);
    else if (c == "clear") post(UI_EV_CMD_CLEAR, raw);
    else if (c == "help")  post(UI_EV_CMD_HELP,  raw);
    else                   post(UI_EV_TEXT,      raw);
}

// --- buttons ----------------------------------------------------------------

typedef struct {
    uint8_t       pin;
    UiEventType_t event;     // what this button emits on a press
    bool          pressed;   // debounced state (true = down)
    bool          raw_last;
    uint32_t      last_change;
} Button;

static Button buttons[4];

static void btn_init(Button *b, uint8_t pin, UiEventType_t ev) {
    b->pin = pin;
    b->event = ev;
    b->pressed = false;
    b->raw_last = false;
    b->last_change = 0;
    pinMode(pin, INPUT);     // input-only pin, external pull-up on the PCB
}

// debounced falling-edge detect. emits the button's event on press
static void btn_poll(Button *b, uint32_t now) {
    bool raw = (digitalRead(b->pin) == LOW);          // active low
    if (raw != b->raw_last) { b->raw_last = raw; b->last_change = now; }
    if ((now - b->last_change) >= BTN_DEBOUNCE_MS && raw != b->pressed) {
        b->pressed = raw;
        if (raw) post(b->event, 0);                   // just went down
    }
}

// --- task -------------------------------------------------------------------

void task_Input(void *pvParameters) {
    btn_init(&buttons[0], BTN_UP,      UI_EV_UP);
    btn_init(&buttons[1], BTN_DOWN,    UI_EV_DOWN);
    btn_init(&buttons[2], BTN_CONFIRM, UI_EV_SELECT);
    btn_init(&buttons[3], BTN_BACK,    UI_EV_BACK);

    String line;
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (read_line(line)) handle_line(line);
        for (int i = 0; i < 4; i++) btn_poll(&buttons[i], now);
        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
    vTaskDelete(NULL);
}
