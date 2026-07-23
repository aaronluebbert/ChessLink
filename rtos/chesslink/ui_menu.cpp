// ui_menu.cpp -- menu table + navigation (layout from the Latest branch)

#include "ui_menu.h"
#include <stddef.h>

const UiScreenDef UI_SCREENS[SCR_COUNT] = {
    /* SCR_MENU     */ { "CHESSLINK", HDR_RED,
                         { "Local", "Online", "Settings" }, 3,
                         { SCR_LOCAL, SCR_ONLINE, SCR_SETTINGS }, SCR_MENU },

    /* SCR_LOCAL    */ { "Local", HDR_BLUE,
                         { "Famous Games", "Local Match", 0 }, 2,
                         { SCR_FAME, UI_LEAF, UI_LEAF }, SCR_MENU },

    /* SCR_ONLINE   */ { "Online", HDR_BLUE,
                         { "Play Stockfish", "Play Player", "Play Ranked" }, 3,
                         { UI_LEAF, UI_LEAF, UI_LEAF }, SCR_MENU },

    /* SCR_FAME     */ { "Fame Game", HDR_BLUE,
                         { "MAGvsGABRIEL", 0, 0 }, 1,
                         { UI_LEAF, UI_LEAF, UI_LEAF }, SCR_LOCAL },

    /* SCR_SETTINGS */ { "Settings", HDR_BLUE,
                         { "Network", "Delete save data", 0 }, 2,
                         { UI_LEAF, SCR_DELETE, UI_LEAF }, SCR_MENU },

    /* SCR_DELETE   */ { "Delete data?", HDR_RED,
                         { "Delete", "Go back", 0 }, 2,
                         { UI_LEAF, SCR_SETTINGS, UI_LEAF }, SCR_SETTINGS },
};

void ui_menu_init(UiMenu *m) { m->current = SCR_MENU; m->selection = 1; }

void ui_menu_up(UiMenu *m) {
    int count = UI_SCREENS[m->current].item_count;
    if (count == 0) return;
    m->selection = (m->selection <= 1) ? count : m->selection - 1;
}

void ui_menu_down(UiMenu *m) {
    int count = UI_SCREENS[m->current].item_count;
    if (count == 0) return;
    m->selection = (m->selection >= count) ? 1 : m->selection + 1;
}

UiSelectResult ui_menu_select(UiMenu *m) {
    UiSelectResult r = { false, false, m->current, m->selection };
    const UiScreenDef *s = &UI_SCREENS[m->current];
    if (s->item_count == 0) return r;
    int dest = s->on_select[m->selection - 1];
    if (dest == UI_LEAF) { r.leaf = true; }
    else { m->current = dest; m->selection = 1; r.navigated = true; }
    return r;
}

bool ui_menu_back(UiMenu *m) {
    int dest = UI_SCREENS[m->current].on_back;
    if (dest == m->current) return false;
    m->current = dest; m->selection = 1;
    return true;
}
