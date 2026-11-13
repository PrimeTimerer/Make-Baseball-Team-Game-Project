// C interface of pcsx2gs.dll (libpcsx2gs.so on Linux): PCSX2's GS (hardware renderer) as the port's GS back end
// (docs/GS_PCSX2.md, phase 2; docs/LINUX.md). urban.exe loads the library at run time (LoadLibrary / dlopen +
// pcsx2gs_get_api), so it starts without it and its own ImGui never meets the copy inside the library. Every call except
// pcsx2gs_get_api must come from one thread (the GS thread).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCSX2GS_API_VERSION 5u

typedef struct Pcsx2GsOpenParams
{
    void *hwnd;             // window to render into (a child of the game window): HWND on Windows, X11 Window (XID) on Linux
    uint32_t width;         // its client size
    uint32_t height;
    float scale;            // internal resolution multiplier (1 = native)
    const char *resources;  // folder holding PCSX2's "shaders/" (UTF-8)
    const char *cache;      // shader cache folder (UTF-8)
    uint32_t vsync;         // 0 = present without waiting, 1 = wait for the monitor (FIFO), 2 = mailbox
    uint32_t stretch;       // 1 = fill the window, 0 = keep the 4:3 aspect (black bars)
    // Image options, "key=value" separated by ';' (unknown keys are ignored; missing ones keep PCSX2's defaults):
    //   fxaa=0/1, texture_filtering=0 nearest / 1 forced bilinear / 2 as the PS2 / 3 forced except sprites,
    //   trilinear=-1 auto / 0 off / 1 as the PS2 / 2 forced, anisotropy=0/2/4/8/16,
    //   cas=0 off / 1 sharpen / 2 sharpen and resize, cas_sharpness=0..100,
    //   shadeboost=0/1, brightness/contrast/saturation/gamma=0..100 (50 = unchanged),
    //   tv_shader=0 none / 1 scanlines / 2 diagonal / 3 triangular / 4 wave / 5 Lottes CRT / 6 4xRGSS / 7 NxAGSS,
    //   present_filter=0 off / 1 smooth / 2 sharp, dithering=0 off / 1 scaled / 2 unscaled / 3 force 32 bit,
    //   blending=0..5 (minimum ... maximum),
    //   texture_dir=<folder holding replacements/> (HD textures, PCSX2's names and formats: PNG, DDS),
    //   texture_precache=0/1 (load all of them at start instead of as the game uses them)
    //   texture_dump=0/1 (with texture_dir: PNGs of the textures the game uses, in <texture_dir>/dumps)
    const char *options;
} Pcsx2GsOpenParams;

// Privileged registers at a vsync, in the order of the PS2 map (0x12000000 + 0x10 * i): PMODE, SMODE1, SMODE2, SRFSH,
// SYNCH1, SYNCH2, SYNCV, DISPFB1, DISPLAY1, DISPFB2, DISPLAY2, EXTBUF, EXTDATA, EXTWRITE, BGCOLOR; then CSR, IMR,
// BUSDIR, SIGLBLID (0x12001000, 1010, 1040, 1080).
typedef struct Pcsx2GsPrivRegs
{
    uint64_t regset1[15];
    uint64_t csr;
    uint64_t imr;
    uint64_t busdir;
    uint64_t siglblid;
} Pcsx2GsPrivRegs;

// In-game menu (UI version 3, API 5): a small immediate-mode UI on top of the image, drawn with the DLL's own ImGui. The draw
// callback set with set_menu runs on the GS thread at every present and builds the menu with these calls (UTF-8
// strings; the bool results say "clicked" / "changed" / "open"). Mods will draw their panels with the same table.
typedef struct Pcsx2GsUi
{
    uint32_t version;
    int (*begin_window)(const char *title, int *open, float width, float height); // centered the first time
    void (*end_window)(void);
    int (*begin_tab_bar)(const char *id);
    void (*end_tab_bar)(void);
    int (*begin_tab)(const char *label);
    void (*end_tab)(void);
    void (*text)(const char *text);
    void (*text_wrapped)(const char *text);
    void (*text_disabled)(const char *text);
    void (*text_colored)(uint32_t rgba, const char *text); // R in the low byte
    void (*heading)(const char *text);                     // larger text for titles
    int (*checkbox)(const char *label, int *value);
    int (*button)(const char *label);
    void (*same_line)(void);
    void (*separator)(void);
    void (*spacing)(void);
    int (*begin_child)(const char *id, float width, float height); // scrolling area (0 = fill)
    void (*end_child)(void);
    void (*push_id)(int id);
    void (*pop_id)(void);
    // version 2
    int (*combo)(const char *label, int *current, const char *const *items, int count); // "##id" hides the label
    int (*radio)(const char *label, int active);
    int (*small_button)(const char *label);
    void (*separator_text)(const char *text);
    void (*indent)(void);
    void (*unindent)(void);
    void (*set_next_width)(float width); // width of the next item (0 = default)
    void (*begin_disabled)(int disabled);
    void (*end_disabled)(void);
    // version 3. Sizes are in the same units as set_next_width (they grow with the menu's text); 0 = fill / default.
    int (*input_int)(const char *label, int *value, int step);                          // typed number; step > 0 adds -/+
    int (*input_text)(const char *label, const char *hint, char *buffer, uint32_t size); // UTF-8; hint shown when empty
    int (*slider_int)(const char *label, int *value, int min, int max);
    int (*selectable)(const char *label, int selected, uint32_t flags); // PCSX2GS_SELECT_*; returns clicked
    int (*begin_table)(const char *id, int columns, uint32_t flags, float width, float height); // PCSX2GS_TABLE_*
    void (*end_table)(void);                                            // only after begin_table returned 1
    void (*table_column)(const char *label, uint32_t flags, float width); // PCSX2GS_COLUMN_*; width 0 = stretch
    void (*table_freeze)(int columns, int rows);                          // header row / first columns stay put
    void (*table_headers)(void);
    void (*table_next_row)(void);
    int (*table_next_column)(void);
    int (*table_sort)(int *column, int *descending); // the current sort (column -1 = none); 1 once after a change
    void (*table_row_color)(uint32_t rgba);          // background of the current row (R in the low byte)
    void (*tooltip)(const char *text);               // for the last item, while the mouse is on it
    int (*item_hovered)(void);
    int (*item_double_clicked)(void);
    int (*collapsing_header)(const char *label, int open_first);
    void (*progress_bar)(float fraction, uint32_t rgba, float width, float height, const char *overlay); // rgba 0 = default
    uint32_t (*key_mods)(void);                    // PCSX2GS_KEY_SHIFT / PCSX2GS_KEY_CTRL while held
    void (*avail)(float *width, float *height);    // room left in the window or child
    void (*same_line_at)(float x);                 // same line, starting at x from the left edge
    int (*button_colored)(const char *label, uint32_t rgba, float width); // rgba 0 = the usual color
    int (*begin_panel)(const char *id, float width, float height, int border); // child area (end_child); negative = fill minus
    void (*clipper_begin)(int count);          // many rows of one height in a scrolling area: draw only the visible ones,
    int (*clipper_step)(int *first, int *end); // while it returns 1: the rows [first, end)
} Pcsx2GsUi;

enum
{
    PCSX2GS_TABLE_SORTABLE = 1u << 0, PCSX2GS_TABLE_SCROLL = 1u << 1, PCSX2GS_TABLE_BORDERS = 1u << 2,
    PCSX2GS_TABLE_ROW_BG = 1u << 3, PCSX2GS_TABLE_RESIZABLE = 1u << 4, PCSX2GS_TABLE_SCROLL_X = 1u << 5,
    PCSX2GS_COLUMN_DEFAULT_SORT = 1u << 0, PCSX2GS_COLUMN_NO_SORT = 1u << 1, PCSX2GS_COLUMN_DESCENDING_FIRST = 1u << 2,
    PCSX2GS_SELECT_SPAN_COLUMNS = 1u << 0, PCSX2GS_SELECT_ALLOW_OVERLAP = 1u << 1, PCSX2GS_SELECT_DOUBLE_CLICK = 1u << 2,
};

typedef void (*Pcsx2GsMenuFn)(const Pcsx2GsUi *ui, void *user);

// Input for the menu, sent by the game window while it is open (any thread; the latest one is used at the next
// frame). Mouse in client pixels; keys and buttons are "held" bits.
typedef struct Pcsx2GsMenuInput
{
    float mouse_x;
    float mouse_y;
    uint32_t mouse_buttons; // bit 0 left, 1 right, 2 middle
    float wheel;            // notches since the last input (positive = up)
    uint32_t keys;          // PCSX2GS_KEY_* bits held
    uint32_t pad;           // PCSX2GS_PAD_* bits held (any controller)
    // API version 5: the characters typed since the last input (UTF-32), for the menu's text fields only
    uint32_t char_count;
    uint32_t chars[16];
} Pcsx2GsMenuInput;

enum
{
    PCSX2GS_KEY_UP = 1u << 0, PCSX2GS_KEY_DOWN = 1u << 1, PCSX2GS_KEY_LEFT = 1u << 2, PCSX2GS_KEY_RIGHT = 1u << 3,
    PCSX2GS_KEY_ENTER = 1u << 4, PCSX2GS_KEY_SPACE = 1u << 5, PCSX2GS_KEY_ESCAPE = 1u << 6, PCSX2GS_KEY_TAB = 1u << 7,
    PCSX2GS_KEY_SHIFT = 1u << 8, PCSX2GS_KEY_CTRL = 1u << 9,
    PCSX2GS_KEY_BACKSPACE = 1u << 10, PCSX2GS_KEY_DELETE = 1u << 11, PCSX2GS_KEY_HOME = 1u << 12, PCSX2GS_KEY_END = 1u << 13,
    PCSX2GS_KEY_PAGE_UP = 1u << 14, PCSX2GS_KEY_PAGE_DOWN = 1u << 15, PCSX2GS_KEY_A = 1u << 16, PCSX2GS_KEY_C = 1u << 17,
    PCSX2GS_KEY_V = 1u << 18, PCSX2GS_KEY_X = 1u << 19, PCSX2GS_KEY_Z = 1u << 20, PCSX2GS_KEY_Y = 1u << 21,
    PCSX2GS_PAD_UP = 1u << 0, PCSX2GS_PAD_DOWN = 1u << 1, PCSX2GS_PAD_LEFT = 1u << 2, PCSX2GS_PAD_RIGHT = 1u << 3,
    PCSX2GS_PAD_ACCEPT = 1u << 4, PCSX2GS_PAD_BACK = 1u << 5, PCSX2GS_PAD_L1 = 1u << 6, PCSX2GS_PAD_R1 = 1u << 7,
};

typedef struct Pcsx2GsApi
{
    uint32_t version;
    // Creates the device and the renderer (DX11 on Windows, Vulkan on Linux). Returns 0 on failure.
    int (*open)(const Pcsx2GsOpenParams *params);
    void (*close)(void);
    // GIF data in stream order (all paths merged), whole qwords; a tag may continue in the next call.
    void (*gif)(const uint8_t *data, uint32_t qwc);
    // End of a field: copies the registers into the GS's register block, composes the output and presents.
    void (*vsync)(const Pcsx2GsPrivRegs *regs);
    // GS reset (CSR.RESET).
    void (*reset)(void);
    // Local -> host transfer started by TRXDIR = 1 (already sent through gif): reads the next qwc qwords.
    void (*read_fifo)(uint8_t *dst, uint32_t qwc);
    // The window's client size changed.
    void (*resize)(uint32_t width, uint32_t height);
    // Internal resolution multiplier.
    void (*set_scale)(float scale);
    // Replaces the GS local memory (4 MB, the GS's own layout) and drops what the renderer derived from it (a save
    // state was loaded). The GS registers are reset too; games send them again with their next draws.
    void (*load_vram)(const uint8_t *data, uint32_t size);
    // Copies the GS local memory (4 MB) into dst, after writing back what only the GPU has (render targets): for a
    // save state. Returns the bytes copied.
    uint32_t (*save_vram)(uint8_t *dst, uint32_t size);
    // Reads a local -> host transfer's area as it is now (draws flushed, render targets written back), without the
    // GS's own transfer state: for reads the port serves from a cache. Returns the bytes written to dst.
    uint32_t (*read_local)(uint8_t *dst, uint32_t size, uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
    // On-screen text drawn over the image at each present (UTF-8; NULL or "" = none): top = a message box at the top
    // left (top_rgba = its background color, R in the low byte; top_alpha fades it), bottom = a line at the bottom left
    // (the FPS counter). Unlike the other calls, any thread may call it.
    void (*set_osd)(const char *top, uint32_t top_rgba, float top_alpha, const char *bottom);
    // Debug: writes the image the GS shows now (internal resolution, before window scaling) as a 24-bit .bmp.
    int (*snapshot)(const char *path);
    // In-game menu: draw = NULL hides it. Any thread.
    void (*set_menu)(Pcsx2GsMenuFn draw, void *user);
    void (*menu_input)(const Pcsx2GsMenuInput *input);
} Pcsx2GsApi;

typedef const Pcsx2GsApi *(*Pcsx2GsGetApiFn)(uint32_t version);

#ifdef __cplusplus
}
#endif
