#ifndef AVP2M_CURSOR_HOOKS_H
#define AVP2M_CURSOR_HOOKS_H

/* Hooks for the Windows cursor calls of the game modules (see cursor_hooks.c). */
#define CURSOR_MAP_RECT  0   /* data: RECT in screen coordinates, as the game passed it */
#define CURSOR_MAP_POINT 1   /* data: long[2] screen point */
typedef void (*cursor_log_fn)(const char *line);
typedef void (*cursor_map_fn)(void *user, int kind, long *data);

void avp2m_cursor_hooks_init(cursor_log_fn log, cursor_map_fn map, void *user);
/* Patch modules not seen yet (lithtech.exe, cshell.dll). Cheap; call every second or so. */
int avp2m_hook_cursor_apis(void);
/* 1 when the game has a cursor shown (menus), 0 in gameplay. */
int avp2m_game_cursor_visible(void);

#endif
