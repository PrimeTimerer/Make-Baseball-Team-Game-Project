/* Urban Reign PC port: mod API (C, version 2).
 *
 * A mod is one DLL in its own folder:   game\mods\<Name>\<Name>.dll  +  mod.json  (name, version, author, description)
 * (Linux: game/mods/<Name>/<Name>.so.) Players switch it on in the launcher (Mods tab; config.ini [Mods]
 * Dlls=<Name>;...). urban.exe loads it at start
 * and calls urban_mod_init with the host table below. Its options appear as a tab of the in-game menu (F2,
 * "MODS / PATCHES").
 *
 * The simplest mod only registers patch groups (PCSX2 .pnach text built into the DLL): each group becomes a checkbox
 * in its tab and is applied while it is on. A mod can also draw its own tab, read and write the game's memory and run
 * code at every frame. See modsdk/example.
 *
 * Threads: urban_mod_init runs on the main thread before the game starts; tab drawing runs on the GS thread; the frame
 * callback runs on the EE (game) thread at every vblank, after the patches. Keep shared state simple (atomics).
 */
#ifndef URBAN_MOD_API_H
#define URBAN_MOD_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define URBAN_MOD_API_VERSION 2u

#if defined(_WIN32)
#define URBAN_MOD_EXPORT __declspec(dllexport)
#else
#define URBAN_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* Immediate-mode UI for a tab (the game's ImGui). The int results are 1 = clicked / changed / open. UTF-8 text. */
typedef struct UrbanModUi
{
    uint32_t version;
    int (*begin_window)(const char *title, int *open, float width, float height); /* host use; mods draw inside a tab */
    void (*end_window)(void);
    int (*begin_tab_bar)(const char *id);
    void (*end_tab_bar)(void);
    int (*begin_tab)(const char *label);
    void (*end_tab)(void);
    void (*text)(const char *text);
    void (*text_wrapped)(const char *text);
    void (*text_disabled)(const char *text);
    void (*text_colored)(uint32_t rgba, const char *text); /* R in the low byte, e.g. 0xFF2ABFE8 = yellow */
    void (*heading)(const char *text);
    int (*checkbox)(const char *label, int *value);
    int (*button)(const char *label);
    void (*same_line)(void);
    void (*separator)(void);
    void (*spacing)(void);
    int (*begin_child)(const char *id, float width, float height); /* always pair with end_child */
    void (*end_child)(void);
    void (*push_id)(int id);
    void (*pop_id)(void);
    /* version 2 */
    int (*combo)(const char *label, int *current, const char *const *items, int count); /* "##id" hides the label */
    int (*radio)(const char *label, int active);
    int (*small_button)(const char *label);
    void (*separator_text)(const char *text);
    void (*indent)(void);
    void (*unindent)(void);
    void (*set_next_width)(float width); /* width of the next item (0 = default) */
    void (*begin_disabled)(int disabled);
    void (*end_disabled)(void);
    /* version 3 (check ui->version >= 3 before use); sizes in set_next_width units, 0 = fill / default */
    int (*input_int)(const char *label, int *value, int step);
    int (*input_text)(const char *label, const char *hint, char *buffer, uint32_t size);
    int (*slider_int)(const char *label, int *value, int min, int max);
    int (*selectable)(const char *label, int selected, uint32_t flags);
    int (*begin_table)(const char *id, int columns, uint32_t flags, float width, float height); /* end_table only after 1 */
    void (*end_table)(void);
    void (*table_column)(const char *label, uint32_t flags, float width);
    void (*table_freeze)(int columns, int rows);
    void (*table_headers)(void);
    void (*table_next_row)(void);
    int (*table_next_column)(void);
    int (*table_sort)(int *column, int *descending);
    void (*table_row_color)(uint32_t rgba);
    void (*tooltip)(const char *text);
    int (*item_hovered)(void);
    int (*item_double_clicked)(void);
    int (*collapsing_header)(const char *label, int open_first);
    void (*progress_bar)(float fraction, uint32_t rgba, float width, float height, const char *overlay);
    uint32_t (*key_mods)(void);
    void (*avail)(float *width, float *height);
    void (*same_line_at)(float x);
    int (*button_colored)(const char *label, uint32_t rgba, float width);
    int (*begin_panel)(const char *id, float width, float height, int border); /* always pair with end_child */
    void (*clipper_begin)(int count);
    int (*clipper_step)(int *first, int *end);
} UrbanModUi;

typedef void (*UrbanModDrawFn)(const UrbanModUi *ui, void *user);
typedef void (*UrbanModFrameFn)(void *user);

typedef struct UrbanModHost
{
    uint32_t version;   /* URBAN_MOD_API_VERSION of the game */
    const char *mod_name;
    const char *language; /* "pt" or "en", as the launcher */

    void (*log)(const char *text);

    /* The PS2's main memory (EE RAM, 32 MB), by PS2 address (0x00100000...). Return 1 on success. */
    int (*read)(uint32_t address, void *out, uint32_t size);
    int (*write)(uint32_t address, const void *data, uint32_t size);

    /* Patch groups: PCSX2 .pnach lines ("patch=1,EE,2059C3A0,extended,00000001"...). Returns an id, or -1 when the text
     * has no patch line. Groups added with show = 1 get a checkbox in the mod's tab; the player's choice is saved. */
    int (*add_patches)(const char *name, const char *description, const char *pnach_text, int enabled, int show);
    void (*set_patches_enabled)(int id, int enabled);
    int (*patches_enabled)(int id);

    /* The mod's tab in the F2 menu, drawn under its patch checkboxes. Optional. */
    void (*set_tab)(UrbanModDrawFn draw, void *user);
    /* Called at every frame (vblank) on the game thread. Optional. */
    void (*on_frame)(UrbanModFrameFn frame, void *user);

    /* The mod's own settings (saved in mods\<Name>\settings.ini). */
    const char *(*get_setting)(const char *key, const char *fallback);
    void (*set_setting)(const char *key, const char *value);

    /* version 2 */
    /* Save states loaded so far (F9...): it changes right before the frame callback that follows a load, so a mod
     * that follows the game from frame to frame can start over. */
    uint32_t (*state_loads)(void);
} UrbanModHost;

/* Exported by the mod: return 1 to stay loaded, 0 to be unloaded (e.g. host->version too old). */
URBAN_MOD_EXPORT int urban_mod_init(const UrbanModHost *host);

#ifdef __cplusplus
}
#endif

#endif
