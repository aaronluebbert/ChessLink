// ui_menu.h -- data-driven menu state machine, hardware-free
//
// menu layout follows the Latest branch (CONNECT_ITALL): Local / Online /
// Settings, with a Settings > Network / Delete flow and a Stockfish option under
// Online. no Arduino deps, so it host-compiles and unit-tests.

#ifndef CHESSLINK_UI_MENU_H
#define CHESSLINK_UI_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAX_ITEMS 3
#define UI_LEAF     (-1)   // onSelect sentinel: item is an action, not a submenu

typedef enum {
    SCR_MENU = 0,
    SCR_LOCAL,
    SCR_ONLINE,
    SCR_FAME,
    SCR_SETTINGS,
    SCR_DELETE,
    SCR_COUNT
} UiScreenId;

typedef enum { HDR_RED = 0, HDR_BLUE } UiHeaderColor;

typedef struct {
    const char*   title;
    UiHeaderColor header_color;
    const char*   items[UI_MAX_ITEMS];
    int           item_count;
    int           on_select[UI_MAX_ITEMS];   // UiScreenId to enter, or UI_LEAF
    int           on_back;
} UiScreenDef;

extern const UiScreenDef UI_SCREENS[SCR_COUNT];

typedef struct { int current; int selection; } UiMenu;

typedef struct {
    bool navigated;
    bool leaf;
    int  screen;
    int  item;
} UiSelectResult;

void ui_menu_init(UiMenu *m);
void ui_menu_up(UiMenu *m);
void ui_menu_down(UiMenu *m);
UiSelectResult ui_menu_select(UiMenu *m);
bool ui_menu_back(UiMenu *m);

#ifdef __cplusplus
}
#endif

#endif
