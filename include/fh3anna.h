/* FH3 ANNA: ANNA's menu (D-pad Down while driving) for other Forza Horizon 3 mods.
 * Copy this file into your mod. Plain C, works from C and C++.
 *
 * ANNA is an Extra Options plugin (mods\Forza Horizon 3\ANNA\plugins\ANNA.dll). Any DLL in the game
 * reaches it the same way, whatever loaded it (an Extra Options plugin, an FH3 Mod Platform mod from
 * fh3-mods, or a mod of its own): ANNA.dll exports FH3ANNA_GetApi, and fh3anna_get() below finds it.
 *
 *   const FH3AnnaApi* anna = fh3anna_get(1);
 *   if (anna) { ... }   // NULL: ANNA isn't installed (or is too old); carry on without it
 *
 * Extra Options loads its plugins in name order once the game's file system is up: a plugin whose name
 * comes after "ANNA" finds it straight away. A mod that starts earlier (an FH3 Mod Platform mod, say)
 * calls fh3anna_wait() from a thread of its own, never from its start-up function or DllMain:
 *
 *   const FH3AnnaApi* anna = fh3anna_wait(1, 60000);   // waits up to a minute for ANNA to load
 *
 * Adding to the menu:
 *
 *   static void honk(void* user, const char* key) { ... }
 *   FH3AnnaEntry e = {sizeof e};
 *   e.key       = "honkhonk.horn";     // unique: start it with your mod's name
 *   e.parent    = FH3ANNA_LIST_CAR;    // or NULL for the main list, or a list of yours
 *   e.label     = "Honk";
 *   e.on_choose = honk;
 *   anna->add_entry(&e);
 *
 * ANNA's own lists: "car" (Car), "car.lights" (Car > Lights), "car.doors" (Car > Doors). An entry
 * without on_choose is a list: its entries are those whose parent is its key, and it only shows when
 * some of them do. Lists open inside ANNA; D-pad Left goes back up one list.
 *
 * on_choose runs on the game's thread (the one that updates the HUD and the cars). Without stays_open,
 * ANNA closes and on_choose runs on the next frame; with it, on_choose runs at once and ANNA shows the
 * same list again, its labels asked again. Keep it short. shown and label_now also run on the game's
 * thread, each time the list opens or is shown again.
 *
 * Labels are UTF-8. ANNA's font has Latin, Cyrillic and Greek letters; other scripts may show as boxes.
 * ANNA translates its own labels into the game's language; translate yours yourself. */
#ifndef FH3ANNA_H
#define FH3ANNA_H

#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define FH3ANNA_EXPORT "FH3ANNA_GetApi"
#define FH3ANNA_DLL L"ANNA.dll"
#define FH3ANNA_LIST_CAR "car"
#define FH3ANNA_LIST_CAR_LIGHTS "car.lights"
#define FH3ANNA_LIST_CAR_DOORS "car.doors"

typedef void (*FH3AnnaChooseFn)(void* user, const char* key);
/* 0: not listed this time. */
typedef int (*FH3AnnaShownFn)(void* user, const char* key);
/* The label now ("Open Roof" or "Close Roof"); the text is copied straight away. */
typedef const char* (*FH3AnnaLabelFn)(void* user, const char* key);

typedef struct FH3AnnaEntry {
    uint32_t size;             /* sizeof(FH3AnnaEntry) */
    const char* key;           /* unique, up to 63 letters, digits, '.', '_', '-' */
    const char* parent;        /* NULL: the main list; else the key of a list */
    const char* label;         /* what it says, UTF-8 */
    FH3AnnaChooseFn on_choose; /* NULL: it's a list */
    FH3AnnaShownFn shown;      /* NULL: always; else asked each time its list opens */
    FH3AnnaLabelFn label_now;  /* NULL: label; else asked each time its list opens */
    void* user;                /* passed back */
    int stays_open;            /* 1: on_choose runs at once and ANNA stays open, the list shown again */
} FH3AnnaEntry;

typedef struct FH3AnnaApi {
    uint32_t size;    /* sizeof(FH3AnnaApi) of the ANNA that gave it */
    uint32_t version; /* 1 */
    /* ---- version 1 ---- */
    /* Adds an entry (copied). Returns 1, or 0 if the key is taken or the entry is incomplete. Add
       entries any time, from any thread; they show from the next time their list opens. */
    int (*add_entry)(const FH3AnnaEntry* entry);
    /* 1 while ANNA's menu is open on the HUD. Any thread. */
    int (*is_open)(void);
    /* 1 while ANNA's Auto Drive drives the player's car. Any thread. */
    int (*is_autodriving)(void);
} FH3AnnaApi;

typedef const FH3AnnaApi* (*FH3AnnaGetApiFn)(uint32_t min_version);

#ifdef _WIN32
/* The API of the ANNA loaded in the game, if it has at least min_version; else NULL. */
static inline const FH3AnnaApi* fh3anna_get(uint32_t min_version) {
    HMODULE anna = GetModuleHandleW(FH3ANNA_DLL);
    FH3AnnaGetApiFn get = anna ? (FH3AnnaGetApiFn)(void*)GetProcAddress(anna, FH3ANNA_EXPORT) : NULL;
    return get ? get(min_version) : NULL;
}

/* fh3anna_get, retried until ANNA has loaded or timeout_ms has passed. Blocks: call it from a thread
   of your own. */
static inline const FH3AnnaApi* fh3anna_wait(uint32_t min_version, uint32_t timeout_ms) {
    ULONGLONG until = GetTickCount64() + timeout_ms;
    for (;;) {
        const FH3AnnaApi* api = fh3anna_get(min_version);
        if (api || GetTickCount64() >= until) return api;
        Sleep(250);
    }
}
#endif

#endif
