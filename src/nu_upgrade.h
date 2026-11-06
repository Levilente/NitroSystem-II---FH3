/* NitroSystem II upgrade: the "Nitrous Kit" part in the garage, bought per owned car, three stages.
   The method is the one of the Universal Wheel Spacers mod, nothing of the game is changed on disk:
     - the type and its 4 option rows go into the game database through the platform (add_sql; the platform
       writes a changed copy to mediaoverride, the original stays as it is),
     - a few SQL statements the game runs are edited in memory (the database query function is hooked):
       our parts join the car's upgrade list, the installed one is marked, the game's "install" write is
       swallowed (the choice is kept per garage id in NitroSystemII.ini),
     - the game's string lookup is answered for our names (they are string references no table has),
     - the game's own getter of the current car's garage id is observed.
   nu_level() is what the rest of the mod reads: 0 = no nitrous on the car being driven, 1..3 = the stage.

   Included by nitro_mod.cpp after windows.h and fh3mod.h; the caller supplies nu_say(). Common C / C++ subset. */
#ifndef NU_UPGRADE_H
#define NU_UPGRADE_H
#include <stdint.h>
#include "nu_sql.h"

#define NU_RVA_DB_GLOBAL       0x034B3640u   /* the game's database object (virtual slot 8 = query) */
#define NU_DB_SLOT             8
#define NU_RVA_LOC_LOOKUP      0x0123D780u   /* string lookup (table, key) -> UTF-16 text */
#define NU_RVA_PROFILE_ROOT    0x034B3E18u   /* global: object whose virtual +0x48 then +0xD8 gives the save-data holder */
#define NU_RVA_CURRENT_GARAGE  0x002DCAB0u   /* (holder) -> garage id of the current car (save value Main/CareerCar) */

static const FH3ModApi *nu_api;
static FH3Mod *nu_mod;
static unsigned char *nu_game;
static wchar_t nu_ini[MAX_PATH + 16];
static NuCfg nu_cfg;
static char nu_db_sql[8000];

static volatile LONG nu_cur_level;       /* stage of the car being driven (0 = none) */
static volatile unsigned nu_active_garage;
static volatile LONG nu_garage_dirty = 1;
static volatile LONG nu_cur_ord;         /* car ordinal of the last UpgradeParts view */
static volatile LONG nu_log_left = 400;
static volatile LONG nu_kind_seen[4];
static uint64_t nu_last_garage_ms;

static void nu_say(const char *t);       /* provided by the including file */

static void nu_say_num(const char *label, long v) {
    char line[96]; size_t o = 0;
    while (label[o] && o < 70) { line[o] = label[o]; o++; }
    char d[16]; int nd = 0;
    if (v < 0) { line[o++] = '-'; v = -v; }
    if (!v) d[nd++] = '0';
    while (v) { d[nd++] = (char)('0' + v % 10); v /= 10; }
    while (nd) line[o++] = d[--nd];
    line[o] = 0;
    nu_say(line);
}

static void nu_say_sql(const char *label, const char *s, size_t n) {
    if (InterlockedExchangeAdd(&nu_log_left, -1) <= 0) return;
    char line[760]; size_t o = 0;
    while (label[o] && o < 40) { line[o] = label[o]; o++; }
    line[o++] = ':'; line[o++] = ' ';
    size_t shown = n < 640 ? n : 640;
    for (size_t i = 0; i < shown; i++) { char c = s[i]; line[o++] = (c == '\r' || c == '\n' || c == '\t') ? ' ' : c; }
    if (n > shown) { line[o++] = '.'; line[o++] = '.'; line[o++] = '.'; }
    line[o] = 0;
    nu_say(line);
}

static int nu_first_few(int kind, int few) { return InterlockedIncrement(&nu_kind_seen[kind]) <= few; }

static int nu_mapped(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION m;
    if (!p || !n || !VirtualQuery(p, &m, sizeof m)) return 0;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return 0;
    uintptr_t a = (uintptr_t)p, e = (uintptr_t)m.BaseAddress + m.RegionSize;
    return a <= UINTPTR_MAX - n && a + n <= e;
}

static size_t nu_cstr_len(const char *s, size_t max) {
    size_t n = 0;
    if (!s) return 0;
    while (n < max) {
        if (n == 0 || (((uintptr_t)(s + n)) & 0xFFF) == 0) { if (!nu_mapped(s + n, 1)) return 0; }
        if (!s[n]) return n;
        n++;
    }
    return 0;
}

/* The game code we expect at `rva`. Another mod may have hooked the same place first (the platform shares a hook
   between mods): then the function starts with MinHook's jump (E9 rel32 / FF 25), or a hooked call is still an
   E8 / E9. That counts as a match; the platform checks every hook against the exe on disk itself. */
static int nu_code_matches(uintptr_t rva, const unsigned char *expected, size_t n) {
    if (!nu_game || !nu_mapped(nu_game + rva, n)) return 0;
    size_t i = 0;
    while (i < n && nu_game[rva + i] == expected[i]) i++;
    if (i == n) return 1;
    const unsigned char *p = nu_game + rva;
    if (p[0] == 0xE9 || (n > 1 && p[0] == 0xFF && p[1] == 0x25)) return 1;
    return expected[0] == 0xE8 && p[0] == 0xE8;
}

/* ---- the ini: [Upgrade] settings, [GarageNitro] "<garage id>=<stage 0..3>", [GarageCar] "<garage id>=<car ordinal>" ---- */
static void nu_wide_num(wchar_t *o, long v) {
    char t[16]; NuBuf b; nu_begin(&b, t, 15); nu_addn(&b, v); t[b.at] = 0;
    for (size_t i = 0; i <= b.at; i++) o[i] = (wchar_t)(unsigned char)t[i];
}

static int nu_parse_tenths(const wchar_t *s, int def) {
    long whole = 0; int i = 0, any = 0, frac = 0;
    while (s[i] == L' ') i++;
    while (s[i] >= L'0' && s[i] <= L'9' && whole < 100) { whole = whole * 10 + (s[i] - L'0'); i++; any = 1; }
    if (s[i] == L'.') { i++; if (s[i] >= L'0' && s[i] <= L'9') { frac = s[i] - L'0'; any = 1; } }
    if (!any) return def;
    long t = whole * 10 + frac;
    return (int)(t < 10 ? 10 : (t > 30 ? 30 : t));
}

/* Loads [Upgrade] (and writes the defaults into the file the first time, so they can be edited). `seconds` = tank size. */
static void nu_load_config(const wchar_t *ini, int seconds) {
    static const long def_price[NU_LEVELS] = { 0, 3000, 6000, 12000 };
    static const int def_tenths[NU_LEVELS] = { 10, 12, 14, 16 };
    nu_ini[0] = 0;
    for (int i = 0; ini && ini[i] && i < MAX_PATH; i++) { nu_ini[i] = ini[i]; nu_ini[i + 1] = 0; }
    nu_cfg.area = 1;
    nu_cfg.seconds = seconds;
    for (int l = 0; l < NU_LEVELS; l++) { nu_cfg.price[l] = def_price[l]; nu_cfg.tenths[l] = def_tenths[l]; }
    if (!nu_ini[0]) return;
    long area = (long)GetPrivateProfileIntW(L"Upgrade", L"Area", 1, nu_ini);
    nu_cfg.area = (area >= 1 && area <= 5) || area == 8 ? (int)area : 1;
    for (int l = 1; l < NU_LEVELS; l++) {
        wchar_t kp[12] = L"Price0", kb[12] = L"Boost0", buf[32], val[16];
        kp[5] = kb[5] = (wchar_t)(L'0' + l);
        long p = (long)GetPrivateProfileIntW(L"Upgrade", kp, -1, nu_ini);
        if (p < 0 || p > 100000000L) { p = def_price[l]; nu_wide_num(val, p); WritePrivateProfileStringW(L"Upgrade", kp, val, nu_ini); }
        nu_cfg.price[l] = p;
        buf[0] = 0;
        GetPrivateProfileStringW(L"Upgrade", kb, L"", buf, 32, nu_ini);
        if (!buf[0]) {
            wchar_t def[8] = { (wchar_t)(L'0' + def_tenths[l] / 10), L'.', (wchar_t)(L'0' + def_tenths[l] % 10), 0 };
            WritePrivateProfileStringW(L"Upgrade", kb, def, nu_ini);
            nu_cfg.tenths[l] = def_tenths[l];
        } else nu_cfg.tenths[l] = nu_parse_tenths(buf, def_tenths[l]);
    }
    wchar_t a[4]; nu_wide_num(a, nu_cfg.area);
    WritePrivateProfileStringW(L"Upgrade", L"Area", a, nu_ini);
}

/* ---- the saved stage per owned car (garage id); a small cache, the ini is read once per car ---- */
typedef struct NuEntry { volatile LONG garage; volatile LONG ord; volatile LONG level; } NuEntry;
static NuEntry nu_cache[32];
static volatile LONG nu_cache_next;

static NuEntry *nu_cache_find(long garage) {
    for (int i = 0; i < 32; i++) if (nu_cache[i].garage == garage) return &nu_cache[i];
    return NULL;
}
static NuEntry *nu_cache_get(long garage) {
    if (garage <= 0 || garage >= 2000000000L) return NULL;
    NuEntry *c = nu_cache_find(garage);
    if (c) return c;
    LONG i = (LONG)((unsigned long)InterlockedIncrement(&nu_cache_next) & 31u);
    c = &nu_cache[i];
    c->garage = 0; c->ord = 0; c->level = 0;
    if (nu_ini[0]) {
        wchar_t k[16]; nu_wide_num(k, garage);
        long lv = (long)GetPrivateProfileIntW(L"GarageNitro", k, 0, nu_ini);
        c->level = lv < 0 ? 0 : (lv >= NU_LEVELS ? NU_LEVELS - 1 : (LONG)lv);
        c->ord = (LONG)GetPrivateProfileIntW(L"GarageCar", k, 0, nu_ini);
    }
    c->garage = (LONG)garage;
    return c;
}

/* levels the player has bought for a car: bit n = stage n (the game forgets a part once another one is installed) */
static long nu_owned_mask(long garage) {
    if (!nu_ini[0] || garage <= 0) return 0;
    wchar_t k[16]; nu_wide_num(k, garage);
    return (long)GetPrivateProfileIntW(L"GarageOwned", k, 0, nu_ini);
}
static void nu_mark_owned(long garage, int lvl) {
    if (!nu_ini[0] || garage <= 0 || lvl <= 0) return;
    long m = nu_owned_mask(garage) | (1L << lvl);
    wchar_t k[16], v[16]; nu_wide_num(k, garage); nu_wide_num(v, m);
    WritePrivateProfileStringW(L"GarageOwned", k, v, nu_ini);
}

static void nu_store(NuEntry *c, long ord, int lvl) {
    c->level = lvl;
    nu_mark_owned((long)c->garage, lvl);
    if (ord > 0) c->ord = (LONG)ord;
    if ((long)c->garage == (long)nu_active_garage) nu_cur_level = lvl;
    if (!nu_ini[0]) return;
    wchar_t k[16], v[16];
    nu_wide_num(k, (long)c->garage); nu_wide_num(v, lvl);
    WritePrivateProfileStringW(L"GarageNitro", k, v, nu_ini);
    if (ord > 0) { nu_wide_num(v, ord); WritePrivateProfileStringW(L"GarageCar", k, v, nu_ini); }
}

/* saved stage of one owned car; `ord` = the model it is (0 = not known, no check). Another model under the same
   garage id (the id of a sold car being reused) means the saved stage is not this car's: forget it. */
static int nu_get_level(long garage, long ord) {
    NuEntry *c = nu_cache_get(garage);
    if (!c) return 0;
    if (ord > 0 && c->ord > 0 && c->ord != ord) { nu_store(c, ord, 0); return 0; }
    return (int)c->level;
}
static void nu_set_level(long garage, long ord, int lvl) {
    NuEntry *c = nu_cache_get(garage);
    if (!c) return;
    nu_store(c, ord, lvl);
}

/* ---- which owned car is being driven ---- */
static void nu_activate(unsigned g) {
    nu_active_garage = g;
    NuEntry *c = nu_cache_get((long)g);
    nu_cur_level = c ? c->level : 0;
    nu_say_num("garage id of the driven car ", (long)g);
    nu_say_num("nitrous stage on it ", (long)nu_cur_level);
}

typedef void *(*NuVirtualGetter)(void *);
typedef unsigned (*NuGarageGetter)(void *);
static NuGarageGetter nu_real_garage_getter;

static unsigned nu_read_current_garage(void) {
    static const unsigned char head[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x05 };
    if (!nu_code_matches(NU_RVA_CURRENT_GARAGE, head, sizeof head)) return 0;
    void *const *rp = (void *const *)(nu_game + NU_RVA_PROFILE_ROOT);
    if (!nu_mapped(rp, 8) || !*rp) return 0;
    void *root = *rp;
    void **vt = nu_mapped(root, 8) ? *(void ***)root : NULL;
    if (!vt || !nu_mapped(vt + 9, 8) || !vt[9]) return 0;                    /* slot 0x48 */
    void *mid = ((NuVirtualGetter)vt[9])(root);
    if (!mid || !nu_mapped(mid, 8)) return 0;
    void **vt2 = *(void ***)mid;
    if (!vt2 || !nu_mapped(vt2 + 27, 8) || !vt2[27]) return 0;               /* slot 0xD8 */
    void *holder = ((NuVirtualGetter)vt2[27])(mid);
    if (!holder || !nu_mapped(holder, 16) || !*(void **)((unsigned char *)holder + 8)) return 0;
    unsigned g = ((NuGarageGetter)(nu_game + NU_RVA_CURRENT_GARAGE))(holder);
    return (g > 0 && g < 2000000000u) ? g : 0;
}

/* The garage id is read only where the game itself reads its save data (the thread that runs the database
   queries), never from a physics or HUD thread, and rarely: every call of the game's own getter is recorded (hooked),
   and the chain is called by us only when the car is unknown or the physics saw another car object (nu_mark_dirty),
   at most once in 2 seconds. (1.5.0 also read it every 3 seconds; the pause menu then froze, so that is gone.) */
static unsigned nu_getter_hook(void *holder) {
    unsigned g = nu_real_garage_getter ? nu_real_garage_getter(holder) : 0;
    if (g > 0 && g < 2000000000u && g != nu_active_garage) nu_activate(g);
    return g;
}
static void nu_mark_dirty(void) { nu_garage_dirty = 1; }
static volatile LONG nu_refresh_logs = 12;
static void nu_refresh_garage(void) {
    if (!nu_garage_dirty && nu_active_garage) return;
    uint64_t now = GetTickCount64();
    if (now - nu_last_garage_ms < 2000u) return;
    nu_last_garage_ms = now;
    nu_garage_dirty = 0;
    if (InterlockedExchangeAdd(&nu_refresh_logs, -1) > 0) nu_say("reading the garage id of the driven car");
    unsigned g = nu_read_current_garage();       /* goes through nu_getter_hook, which records it */
    if (g && g != nu_active_garage) nu_activate(g);
}

/* ---- buffers for rewritten statements: a ring, so the game can still read the one it was given after the call returned ---- */
static char *volatile nu_ring[16];
static volatile LONG nu_ring_i;
static char *nu_alloc(size_t cap) { return (char *)HeapAlloc(GetProcessHeap(), 0, cap); }
static void nu_drop(char *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static const char *nu_publish(char *buf) {
    LONG i = (LONG)((unsigned long)InterlockedIncrement(&nu_ring_i) & 15u);
    char *old = nu_ring[i];
    nu_ring[i] = buf;
    nu_drop(old);
    return buf;
}

enum { NU_K_VIEW, NU_K_UPDATE, NU_K_LIST, NU_K_OTHER };

/* Returns the statement to run: `sql` itself, or a rewritten copy. */
static const char *nu_rewrite(const char *sql, size_t len) {
    if (len < 24) return sql;
    NuBuf b; char *buf; long ord, garage; int lvl;
    if (sql[0] == 'C' && nu_starts(sql, len, "CREATE VIEW")) {
        buf = nu_alloc(len + 2048);
        if (!buf) return sql;
        nu_begin(&b, buf, len + 2047);
        if (nu_view(sql, len, &b, &ord, &nu_cfg)) {
            buf[b.at] = 0;
            nu_cur_ord = (LONG)ord;
            if (nu_first_few(NU_K_VIEW, 3)) nu_say_num("upgrade list built for car ordinal ", ord);
            return nu_publish(buf);
        }
        nu_drop(buf);
        return sql;
    }
    if (sql[0] == 'U') {
        buf = nu_alloc(len + 256);
        if (!buf) return sql;
        nu_begin(&b, buf, len + 255);
        if (nu_update(sql, len, &b, &lvl, &garage)) {
            buf[b.at] = 0;
            nu_set_level(garage, nu_cur_ord, lvl);
            if (nu_first_few(NU_K_UPDATE, 12)) {
                nu_say_sql("install nitrous kit", sql, len);
                nu_say_num("  stage ", lvl);
                nu_say_num("  garage id ", garage);
                nu_say_num("  car ordinal ", nu_cur_ord);
            }
            return nu_publish(buf);
        }
        nu_drop(buf);
        return sql;
    }
    if (sql[0] == 'S') {
        if (nu_starts(sql, len, "SELECT 1 From Profile0_Career_PurchasedParts") && nu_stock_check(sql, len)) {
            /* "is this part bought?" for our STOCK entry: always yes (the game has no part object for our type) */
            if (nu_first_few(NU_K_OTHER, 6)) nu_say_sql("stock entry counts as bought", sql, len);
            return "SELECT 1";
        }
        if (nu_starts(sql, len, "SELECT PartTable.PartId AS id") && nu_partlist_enum(sql, len, &garage)) {
            lvl = nu_get_level(garage, nu_cur_ord);
            buf = nu_alloc(len + 64);
            if (!buf) return sql;
            nu_begin(&b, buf, len + 63);
            if (nu_partlist(sql, len, &b, lvl)) {
                buf[b.at] = 0;
                if (nu_first_few(NU_K_LIST, 4)) { nu_say_sql("part list", sql, len); nu_say_num("  installed stage ", lvl); nu_say_num("  garage id ", garage); }
                return nu_publish(buf);
            }
            nu_drop(buf);
            if (nu_first_few(NU_K_OTHER, 6)) nu_say_sql("part list NOT recognised", sql, len);
            return sql;
        }
    }
    return sql;
}

typedef void *(*NuQueryFn)(void *, void **, const char *);
static NuQueryFn nu_real_query;
static void *nu_query_hook(void *self, void **out, const char *sql) {
    nu_refresh_garage();
    size_t len = nu_cstr_len(sql, 0x40000);
    const char *use = sql;
    if (len) use = nu_rewrite(sql, len);
    return nu_real_query ? nu_real_query(self, out, use) : NULL;
}

/* ---- our texts: the game's string lookup (table, key) -> UTF-16 text; for our keys, which no table has,
   it returns our text instead of "" ---- */
typedef const uint16_t *(*NuLocLookup)(void *, unsigned, unsigned);
static NuLocLookup nu_real_lookup;
static uint16_t nu_texts[NU_KEY_LAST - NU_KEY_FIRST + 1][100];
static void nu_build_texts(void) {
    char tmp[100];
    for (unsigned k = NU_KEY_FIRST; k <= NU_KEY_LAST; k++) {
        const char *t = nu_key_text(k, tmp, &nu_cfg);
        uint16_t *o = nu_texts[k - NU_KEY_FIRST];
        int i = 0;
        if (t) while (t[i] && i < 99) { o[i] = (uint16_t)(unsigned char)t[i]; i++; }
        o[i] = 0;
    }
}
static const uint16_t *nu_lookup_hook(void *mgr, unsigned table, unsigned key) {
    const uint16_t *r = nu_real_lookup ? nu_real_lookup(mgr, table, key) : NULL;
    unsigned k = key & 0xFFFFu;
    if (k >= NU_KEY_FIRST && k <= NU_KEY_LAST && nu_texts[k - NU_KEY_FIRST][0] && (!r || !r[0])) return nu_texts[k - NU_KEY_FIRST];
    return r;
}

static DWORD WINAPI nu_install_worker(LPVOID unused) {
    (void)unused;
    nu_game = (unsigned char *)nu_api->wait_for_code(0xFFFFFFFFu);
    if (!nu_game) { nu_say("the game's code was not ready; the nitrous upgrade is not active"); return 0; }
    static const unsigned char lookup_head[] = { 0x48, 0x83, 0xEC, 0x38, 0xB8, 0xFF, 0xFF, 0x00, 0x00, 0x45, 0x8B, 0xC8, 0x66, 0x3B, 0xD0 };
    static const unsigned char garage_head[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x05 };
    nu_build_texts();
    FH3Hook *hl = NULL, *hg = NULL;
    if (nu_code_matches(NU_RVA_LOC_LOOKUP, lookup_head, sizeof lookup_head))
        hl = nu_api->hook_function(nu_mod, "string lookup (nitrous kit names)", nu_game + NU_RVA_LOC_LOOKUP, (void *)nu_lookup_hook, (void **)&nu_real_lookup);
    if (hl && nu_api->enable_hooks(nu_mod, hl)) nu_say("names: Nitrous Kit, Stock, Stage 1..3 Nitrous");
    else nu_say("the game's string lookup is not the expected code: the nitrous upgrade names stay empty");
    if (nu_code_matches(NU_RVA_CURRENT_GARAGE, garage_head, sizeof garage_head))
        hg = nu_api->hook_function(nu_mod, "current garage id (game getter)", nu_game + NU_RVA_CURRENT_GARAGE, (void *)nu_getter_hook, (void **)&nu_real_garage_getter);
    if (hg) nu_api->enable_hooks(nu_mod, hg);
    void **slot_vt = NULL;
    for (int tries = 0; tries < 1200 && !slot_vt; tries++) {          /* the database object exists a moment after start */
        void *const *gp = (void *const *)(nu_game + NU_RVA_DB_GLOBAL);
        if (nu_mapped(gp, 8) && *gp && nu_mapped(*gp, 8)) {
            void **vt = *(void ***)*gp;
            if (nu_mapped(vt, (NU_DB_SLOT + 1) * sizeof(void *)) && vt[NU_DB_SLOT] && nu_mapped(vt[NU_DB_SLOT], 16)) slot_vt = vt;
        }
        if (!slot_vt) Sleep(500);
    }
    if (!slot_vt) { nu_say("the game's database object was not found; the nitrous upgrade is not active"); return 0; }
    FH3Hook *h = nu_api->hook_function(nu_mod, "database query (nitrous kit)", slot_vt[NU_DB_SLOT], (void *)nu_query_hook, (void **)&nu_real_query);
    if (!h || !nu_api->enable_hooks(nu_mod, h)) { nu_say("could not hook the database query function; the nitrous upgrade is not active"); return 0; }
    nu_say("nitrous upgrade active: Garage > Upgrades > Custom upgrade > Engine > Nitrous Kit");
    return 0;
}

/* Called from fh3mod_init (after load_config). Needs the platform API with add_sql and hooks (api_version >= 3). */
/* The mod's own upgrade icons (the PNGs in icons/, packed into icon_data.h) are handed to the game as files under
   media\ui\textures\data_bound\Upgrade_Parts.zip\Custom\ through the platform (set_file): nothing of the game
   is changed on disk, the files are mediaoverride copies that the platform removes again with the mod. */
static void nu_install_icons(const FH3ModApi *api, FH3Mod *mod) {
    static const char base[] = "ui/textures/data_bound/Upgrade_Parts/";
    if (ICON_COUNT == 0 || api->api_version < 2 || !api->set_file) return;
    int ok = 0, total = 0;
    for (const IconEntry *e = k_icons; e->slot; e++) {
        char path[200]; int n = 0;
        for (int i = 0; base[i]; i++) path[n++] = base[i];
        icon_name(e, path + n, '/');
        unsigned size = icon_unpack(e, g_icon_buf);
        total++;
        if (api->set_file(mod, path, g_icon_buf, size)) ok++;
    }
    char msg[96] = "upgrade icons handed to the game: ";
    int m = 0; while (msg[m]) m++;
    msg[m++] = (char)('0' + (ok / 10) % 10); msg[m++] = (char)('0' + ok % 10);
    msg[m++] = '/'; msg[m++] = (char)('0' + (total / 10) % 10); msg[m++] = (char)('0' + total % 10); msg[m] = 0;
    nu_say(msg);
}

static int nu_init(const FH3ModApi *api, FH3Mod *mod, const wchar_t *ini, int seconds) {
    nu_api = api; nu_mod = mod;
    if (!api || api->api_version < 3 || api->size < sizeof(FH3ModApi) || !api->hook_function || !api->enable_hooks || !api->wait_for_code || !api->add_sql) return 0;
    nu_load_config(ini, seconds);
    nu_install_icons(api, mod);
    NuBuf b; nu_begin(&b, nu_db_sql, sizeof nu_db_sql - 1);
    if (!nu_database_sql(&b, &nu_cfg)) { nu_say("internal error: the database statements do not fit"); return 0; }
    nu_db_sql[b.at] = 0;
    api->add_sql(mod, nu_db_sql);
    HANDLE t = CreateThread(NULL, 0, nu_install_worker, NULL, 0, NULL);
    if (!t) { nu_say("could not start the installer thread"); return 0; }
    CloseHandle(t);
    return 1;
}

/* what the rest of the mod reads */
static volatile LONG nu_foreign_car;   /* 1 = the car being driven is not the garage car the stage belongs to (a mission loaner) */
static int nu_level(void) { if (nu_foreign_car) return 0; LONG v = nu_cur_level; return v < 0 ? 0 : (v >= NU_LEVELS ? NU_LEVELS - 1 : (int)v); }
static float nu_boost(int level) { return level <= 0 ? 1.0f : (float)nu_cfg.tenths[level >= NU_LEVELS ? NU_LEVELS - 1 : level] / 10.0f; }
#endif
