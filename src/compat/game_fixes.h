#ifndef AVP2M_GAME_FIXES_H
#define AVP2M_GAME_FIXES_H

/* In-memory fixes for bugs in the original game modules (see game_fixes.c).
 * Cheap enough to call often (a byte compare per loaded module); a fix is
 * applied only where the original bytes match exactly. Returns the number of
 * fixes found applied (newly or earlier). */
typedef void (*game_fix_log_fn)(const char *name, void *where);
int avp2m_apply_game_fixes(game_fix_log_fn log);
void avp2m_disable_game_fixes(int disabled);
/* Start a background thread that applies the fixes as soon as a module loads. */
void avp2m_start_game_fix_watcher(void);
/* 1 if the watcher applied a fix since the last call (for logging). */
int avp2m_game_fix_news(const char **name, void **where);

#endif
