#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace geode::prelude;

// ───────────────────────── settings helpers ─────────────────────────
static bool optd(char const* k, bool d) { return Mod::get()->getSavedValue<bool>(k, d); }
static bool opt(char const* k) { return optd(k, false); }
static void setOpt(char const* k, bool v) { Mod::get()->setSavedValue<bool>(k, v); }
static double getVal(char const* k, double d) { return Mod::get()->getSavedValue<double>(k, d); }
static void setVal(char const* k, double v) { Mod::get()->setSavedValue<double>(k, v); }
static std::string getStr(char const* k, std::string d) { return Mod::get()->getSavedValue<std::string>(k, d); }
static void setStr(char const* k, std::string v) { Mod::get()->setSavedValue<std::string>(k, v); }

// Audio Speed / Pitch Shift / Sync with Speedhack: scales the pitch (and so the speed) of the level music.
static void applyAudio(bool inLevel) {
    static bool changed = false;
    auto fm = FMODAudioEngine::sharedEngine();
    if (!fm || !fm->m_backgroundMusicChannel) return;
    float p = 1.f;
    if (inLevel) {
        if (opt("audiospeed")) p *= (float)std::clamp(getVal("audiospeed_v", 1.0), 0.1, 4.0);
        if (opt("pitchshift")) p *= (float)std::clamp(getVal("pitchshift_v", 1.0), 0.1, 4.0);
        if (opt("syncwithspeedhack") && opt("speedhack")) p *= (float)std::clamp(getVal("speed", 1.0), 0.1, 4.0);
    }
    if (p != 1.f || changed) {
        fm->m_backgroundMusicChannel->setPitch(p);
        changed = p != 1.f;
    }
}

// Physics Bypass (best effort, Windows + Android only; does nothing on macOS)
static bool s_tpsInner = false;
static double s_tpsAcc = 0.0;
static double tpsVal() { return std::clamp(getVal("physicsbypass_v", 240.0), 30.0, 10000.0); }
static bool tpsActive(GJBaseGameLayer* gl) {
#ifdef GEODE_IS_MACOS
    return false;
#else
    if (!opt("physicsbypass") || std::fabs(tpsVal() - 240.0) < 0.5) return false;
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == gl;
#endif
}

static bool s_confirmBypass = false;
static bool s_audioMuted = false;   // music paused by Mute Level Audio on Death // lets a confirmed pause-menu action through

static constexpr size_t TRAIL_LEN = 240;
static constexpr size_t TRAIL_STEP = 3;
static constexpr float ROWH = 28.3f;

static float s_fps = 60.f;
static float s_hue = 0.f;
static std::deque<double> s_clicks;
static bool s_forceKill = false;   // lets our own accuracy/death limits kill through noclip
static bool s_extBlock = false;    // a death was blocked by the PlayerObject fallback
static int s_attemptClicks = 0;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ───────────────────────── colours ─────────────────────────
static ccColor3B const COLS[9] = {
    {0, 220, 255}, {80, 255, 100}, {255, 70, 70}, {255, 160, 40}, {255, 230, 60},
    {180, 90, 255}, {255, 100, 200}, {70, 120, 255}, {255, 255, 255}
};
static std::vector<char const*> const COLNAMES = {
    "Cyan", "Green", "Red", "Orange", "Yellow", "Purple", "Pink", "Blue", "White"
};
static std::vector<char const*> const ACCNAMES = {
    "Default", "Cyan", "Green", "Red", "Orange", "Yellow", "Purple", "Pink", "Blue", "White", "Rainbow"
};

// configurable keyboard shortcuts (index 0 = Off)
static std::vector<char const*> const KEYNAMES = {
    "Off", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Z", "X", "C", "V"
};
static enumKeyCodes const SCKEYS[17] = {
    KEY_Unknown, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_F11, KEY_F12, KEY_Z, KEY_X, KEY_C, KEY_V
};
static bool keyIs(enumKeyCodes k, char const* valKey, int def) {
    int i = std::clamp((int)getVal(valKey, def), 0, 16);
    return i > 0 && SCKEYS[i] == k;
}

// menu palette (matches the screenshots)
static ccColor3B const C_PANEL {38, 40, 66};
static ccColor3B const C_SIDE  {33, 35, 58};
static ccColor3B const C_BTN   {58, 61, 88};
static ccColor3B const C_INPUT {27, 28, 46};
static ccColor3B const C_SEL   {200, 206, 255};
static ccColor3B const C_DARK  {28, 30, 50};
static ccColor3B const C_WHITE {255, 255, 255};

static ccColor3B hsv(float h) {
    h = h - std::floor(h);
    float f = h * 6.f - std::floor(h * 6.f), q = 1.f - f, r, g, b;
    switch ((int)(h * 6.f) % 6) {
        case 0: r = 1; g = f; b = 0; break;
        case 1: r = q; g = 1; b = 0; break;
        case 2: r = 0; g = 1; b = f; break;
        case 3: r = 0; g = q; b = 1; break;
        case 4: r = f; g = 0; b = 1; break;
        default: r = 1; g = 0; b = q; break;
    }
    return {(GLubyte)(r * 255), (GLubyte)(g * 255), (GLubyte)(b * 255)};
}
static ccColor3B colIdx(char const* key, int def) {
    return COLS[std::clamp((int)getVal(key, def), 0, 8)];
}
static ccColor3B accentColor() {
    int i = (int)getVal("accent", 0);
    if (i <= 0) return C_SEL;
    if (i >= 10) return hsv(s_hue);
    return COLS[std::clamp(i - 1, 0, 8)];
}
static ccColor4F f4(ccColor3B c, float a) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f, a}; }

static void drawRect(CCDrawNode* n, CCRect const& rc, ccColor4F col, bool fill, float th) {
    CCPoint v[4] = {
        {rc.getMinX(), rc.getMinY()}, {rc.getMaxX(), rc.getMinY()},
        {rc.getMaxX(), rc.getMaxY()}, {rc.getMinX(), rc.getMaxY()}
    };
    ccColor4F fc = fill ? ccColor4F{col.r, col.g, col.b, col.a * 0.25f} : ccColor4F{0, 0, 0, 0};
    n->drawPolygon(v, 4, fc, th, col);
}

// ───────────────────────── labels (in-game) ─────────────────────────
struct LblDef { char const* id; char const* name; };
static std::vector<LblDef> const LBLS = {
    {"fps", "FPS"}, {"testmode", "Testmode"}, {"runfrom", "Run From"}, {"attempt", "Attempt"},
    {"pct", "Percentage"}, {"time", "Level Time"}, {"best", "Best Run"}, {"clock", "Clock"},
    {"cps", "CPS"}, {"nca", "Noclip Accuracy"}, {"ncd", "Noclip Deaths"},
    {"jumps", "Jumps"}, {"level", "Level Name"}, {"bot", "Bot"},
    {"frame", "Frame"}, {"pos", "Position"}, {"yvel", "Y Velocity"}, {"speed", "Speed"}, {"tps", "TPS"}
};
static std::string lblName(std::string const& id) {
    for (auto& d : LBLS) if (id == d.id) return d.name;
    return id;
}
static std::vector<std::string> s_order;
static bool s_orderLoaded = false;
static std::vector<std::string>& lblOrder() {
    if (!s_orderLoaded) {
        std::string def;
        for (auto& d : LBLS) { def += d.id; def += ","; }
        std::string s = getStr("lblorder", def);
        std::string cur;
        for (char c : s) {
            if (c == ',') { if (!cur.empty()) s_order.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) s_order.push_back(cur);
        if (getVal("lblver", 0.0) < 2.0) { // add labels introduced after the first version
            for (auto& d : LBLS)
                if (std::find(s_order.begin(), s_order.end(), std::string(d.id)) == s_order.end()) s_order.push_back(d.id);
            std::string o;
            for (auto& i : s_order) { o += i; o += ","; }
            setStr("lblorder", o);
            setVal("lblver", 2.0);
        }
        s_orderLoaded = true;
    }
    return s_order;
}
static void saveOrder() {
    std::string s;
    for (auto& i : s_order) { s += i; s += ","; }
    setStr("lblorder", s);
}

// ───────────────────────── menu definition ─────────────────────────
struct Feat {
    enum K { Toggle, Input, Stepper, Button, Note };
    K k = Toggle;
    std::string label, key, info, gear;
    bool def = false;
    std::string valKey;
    double vdef = 0, vmin = 0, vmax = 0, step = 0;
    std::vector<char const*> names;
};
struct Tab { char const* name; std::vector<Feat> feats; };

static std::string autoKey(std::string const& s) {
    std::string r;
    for (char c : s) if (std::isalnum((unsigned char)c)) r += (char)std::tolower((unsigned char)c);
    return r;
}
// toggle (key derived from label)
static Feat Tg(std::string label, std::string info = "", std::string gear = "", bool def = false) {
    Feat f; f.k = Feat::Toggle; f.label = label; f.key = autoKey(label);
    f.info = info; f.gear = gear; f.def = def; return f;
}
// toggle with explicit key
static Feat TK(char const* key, std::string label, bool def = false) {
    Feat f; f.k = Feat::Toggle; f.label = label; f.key = key; f.def = def; return f;
}
// toggle + number text box
static Feat Inp(std::string label, char const* valKey, double vdef, std::string info = "", std::string gear = "") {
    Feat f; f.k = Feat::Input; f.label = label; f.key = autoKey(label);
    f.valKey = valKey; f.vdef = vdef; f.info = info; f.gear = gear; return f;
}
// stepper only
static Feat Sp(std::string label, char const* valKey, double mn, double mx, double st, double vd,
               std::vector<char const*> names = {}) {
    Feat f; f.k = Feat::Stepper; f.label = label; f.valKey = valKey;
    f.vmin = mn; f.vmax = mx; f.step = st; f.vdef = vd; f.names = std::move(names); return f;
}
// toggle + stepper
static Feat St(std::string label, char const* key, char const* valKey, double mn, double mx, double st, double vd) {
    Feat f = Sp(label, valKey, mn, mx, st, vd); f.key = key; return f;
}
static Feat Bt(std::string label, char const* key) {
    Feat f; f.k = Feat::Button; f.label = label; f.key = key; return f;
}
static Feat Nt(std::string text) {
    Feat f; f.k = Feat::Note; f.label = text; return f;
}

static std::vector<Tab> const& tabs() {
    static std::vector<Tab> t = {
        {"Global", {
            Inp("Physics Bypass", "physicsbypass_v", 240),
            Inp("Speedhack", "speed", 1, "Changes the game speed while in a level.", "speedhack"),
            Inp("Pitch Shift", "pitchshift_v", 1),
            Inp("Audio Speed", "audiospeed_v", 1),
            Tg("Sync with Speedhack"),
            Tg("Auto Safe Mode", "Turns on Safe Mode automatically once a cheat is used.", "autosafe", true),
            Tg("Click Between Steps"),
            Tg("Click On Steps"),
            Tg("Compact Editor Levels"),
            Tg("Compact Profile Comments"),
            Tg("No Short Numbers", "", "nsn"),
            Tg("Safe Mode", "Completions and progress are not saved.", "safemode"),
            Tg("Show Taps", "", "taps"),
        }},
        {"Level", {
            Tg("0% Practice Completion"),
            Tg("Accurate Percentage", "Shows two decimals on the percentage label.", "accpct"),
            Tg("All Modes in Platformer"),
            Tg("All Passable"),
            Tg("Always Show Coins"),
            Tg("Auto LDM", "Turns on Low Detail Mode."),
            Tg("Auto Pickup Coins"),
            Tg("Auto Practice Mode", "Enters practice mode when a level starts."),
            Tg("Auto Song Download"),
            Tg("Checkpoint Delay"),
            Tg("Confirm Full Restart"),
            Tg("Confirm Practice Mode", "", "cpm"),
            Tg("Confirm Quit"),
            Tg("Death Percent Popup"),
            Tg("Confirm Restart"),
            Tg("Force Platformer"),
            Tg("Freeze Attempts", "The attempt counter stops increasing."),
            Tg("Hide Level Complete VFX"),
            Tg("Hide Pause Menu"),
            Tg("Hitbox Multiplier", "", "hbm"),
            Tg("Instant Mirror"),
            Tg("Layout Mode", "", "layout"),
            Tg("Legacy Pulse", "", "lpulse"),
            Tg("Legacy Reverse Physics"),
            Tg("Legacy Trail", "", "ltrail"),
            Tg("Match Percent/Time Fonts"),
            Tg("Mute Level SFX"),
            Tg("No End Shake"),
            Tg("No Mirror"),
            Tg("No Music Fade Out"),
            Tg("No Object Glow"),
            Tg("No Particles", "", "noparticles"),
            Tg("No Portal Lightning"),
            Tg("No Shaders"),
            Tg("Pause Countdown", "", "pausecd"),
            Tg("Pause During Complete"),
            Tg("Practice Coins"),
            Tg("Practice Fix"),
            Tg("Random Seed", "", "rseed"),
            Tg("Show Hitboxes", "Draws object and player hitboxes.", "hitbox"),
            Tg("Hitboxes On Death", "Shows hitboxes only after you die.", "hitbox"),
            Tg("Show Triggers"),
            Tg("Smart StartPos"),
            Tg("StartPos Switcher", "", "spswitch"),
            Tg("Stop Triggers on Death", "", "stopdeath"),
        }},
        {"Bypass", {
            Tg("Allow Low Volume"), Tg("Character Filter Bypass"),
            Tg("Character Limit Bypass"), Tg("Checkpoint Limit"),
            Tg("Quick Chests"), Tg("Instant Complete"),
            Tg("Level Kick"), Tg("Menu Gameplay"),
            Tg("Music Customizer"), Tg("Pause Buffering"),
            Tg("Practice Music Sync"), Tg("Show Spam Comments"),
            Tg("Swift Click"), Tg("Unlock Icons", "Every icon and colour is unlocked.", "unlockicons"),
            Tg("Unlock Main Levels"), Tg("Unlock Paths"),
            Tg("Unlock Shops"), Tg("Unlock Vaults"),
        }},
        {"Player", {
            Tg("Always Show Trail"), Inp("AutoClicker", "autoclickcps", 10),
            Inp("Auto Kill", "autokillpct", 100), Tg("Ball Rotation Bug", "", "ballrot"),
            Tg("Custom Wave Trail", "", "cwt"), Tg("Force Ghost Trail"),
            Tg("Frame Stepper", "", "fstep"), Tg("Hide Player", "Makes the player invisible.", "hideplr"),
            Tg("Jump Hack", "Lets you jump at any time.", "jumphack"), Tg("Mute Level Audio on Death"),
            Tg("Mute Rewards SFX on Death"), Tg("No Death Effect", "Skips the death explosion.", "nde"),
            Tg("No Ghost Trail"), Tg("No Hide Trigger"),
            Tg("No Platformer Jump Anim"), Tg("No Respawn Flash"),
            Tg("No Robot Fire"), Tg("No Spider Dash"),
            Tg("No Trail", "", "notrail"), Tg("No Vehicle Particles"),
            Tg("No Wave Trail Behind"), Tg("Noclip", "Take no damage from hazards and solids.", "noclip"),
            Tg("Shipcopter"), Tg("Show Trajectory", "", "traj"),
            Tg("Solid Wave Trail"), Tg("Wave Trail On Death", "", "wtod"),
            Inp("Respawn Delay", "respawndelay_v", 1),
        }},
        {"Bot", {
            Sp("Bot Mode", "botmode", 0, 2, 1, 0, {"Off", "Record", "Replay"}),
            Tg("Use Anchors", "", "anchors", true),
            Tg("Show Anchors"),
            Bt("Add Anchor Now", "bot_anchor"),
            Bt("Clear Macro", "bot_clear"),
            Bt("Save Macro", "bot_save"),
            Bt("Load Macro", "bot_load"),
        }},
        {"Creator", { Nt("Nothing here yet.") }},
        {"Labels", {}},   // custom page
        {"Shortcuts", {
            Bt("Restart Level", "sc_restart"),
            Bt("Toggle Practice Mode", "sc_practice"),
            Bt("Respawn From Start", "sc_full"),
            Bt("Toggle Hitboxes", "sc_hitbox"),
            Bt("Toggle Noclip", "sc_noclip"),
            Nt("Keyboard keys (Windows)"),
            Sp("Restart Key", "key_restart", 0, 16, 1, 3, KEYNAMES),
            Sp("Practice Key", "key_practice", 0, 16, 1, 4, KEYNAMES),
            Sp("Full Restart Key", "key_full", 0, 16, 1, 0, KEYNAMES),
            Sp("Hitbox Key", "key_hitbox", 0, 16, 1, 2, KEYNAMES),
            Sp("Noclip Key", "key_noclip", 0, 16, 1, 1, KEYNAMES),
        }},
        {"Interface", {
            Sp("Accent", "accent", 0, 10, 1, 0, ACCNAMES),
            Sp("Menu Opacity", "menuop", 0.2, 1, 0.1, 0.95),
            Sp("Dot Size", "dotsize", 10, 30, 2, 18),
            Sp("Dot Opacity", "dotop", 0.1, 1, 0.1, 0.6),
            TK("togglenotifications", "Toggle Notifications"),
            TK("holdinfo", "Hold For Descriptions", true),
            Sp("Hold Time", "holdtime", 0.2, 1.5, 0.1, 0.5),
            Bt("Reset Dot Position", "resetdot"),
            Bt("Reset Style", "resetstyle"),
        }},
    };
    return t;
}
static int const LABELS_TAB = 5;

// sub-pages opened with the gear buttons
static std::vector<Feat> const& subPage(std::string const& id) {
    static std::map<std::string, std::vector<Feat>> m = {
        {"noclip", {
            TK("noclip_p1", "Noclip P1 Only"),
            TK("noclip_p2", "Noclip P2 Only"),
            TK("ncflash", "Noclip Flash"),
            Sp("Flash Color", "flashcol", 0, 8, 1, 2, COLNAMES),
            Sp("Flash Opacity", "flashop", 0.1, 1, 0.1, 0.4),
            TK("nc_practice", "Only In Practice"),
            TK("nc_hazonly", "Pass Hazards Only"),
            TK("nc_solidonly", "Pass Solids Only"),
            TK("nc_redhit", "Red Hitbox On Block"),
            St("Start At %", "nc_after", "ncafter", 0, 100, 5, 0),
            St("Stop At %", "nc_until", "ncuntil", 0, 100, 5, 100),
            St("Min Accuracy %", "nclimit", "nclimitv", 50, 100, 1, 95),
            St("Max NC Deaths", "ncdlimit", "ncdlimitv", 1, 100, 1, 10),
        }},
        {"hitbox", {
            TK("fill", "Fill Hitboxes"),
            TK("trail", "Hitbox Trail"),
            St("Trail Length", "customtrail", "traillen", 30, 600, 30, 240),
            Sp("Hazard Color", "hazcol", 0, 8, 1, 2, COLNAMES),
            Sp("Solid Color", "solidcol", 0, 8, 1, 7, COLNAMES),
            Sp("Player Color", "plrcol", 0, 8, 1, 4, COLNAMES),
            Sp("Trail Color", "trailcol", 0, 8, 1, 2, COLNAMES),
            Sp("Line Thickness", "hbthick", 0.5, 3, 0.5, 1),
        }},
        {"speedhack", { TK("speed_practice", "Only In Practice") }},
        {"autosafe", {
            TK("asm_noclip", "Trigger On Noclip", true),
            TK("asm_jump", "Trigger On Jump Hack", true),
            TK("asm_speed", "Trigger On Speedhack", true),
            TK("asm_bot", "Trigger On Bot Replay", true),
        }},
        {"safemode", { TK("sm_notify", "Warn On Level Complete") }},
        {"accpct", { Sp("Decimals", "accdec", 1, 3, 1, 2) }},
        {"jumphack", { TK("jh_p1only", "Player 1 Only"), TK("jh_practice", "Only In Practice") }},
        {"hideplr", { TK("hp_keepp1", "Keep Player 1 Visible"), TK("hp_keepp2", "Keep Player 2 Visible") }},
        {"nde", { TK("nde_practice", "Only In Practice") }},
        {"notrail", { TK("nt_p2", "Also Player 2", true) }},
        {"unlockicons", { TK("ui_colors", "Include Colors", true) }},
        {"cheat", {
            Sp("Dot Size", "chdot", 2, 12, 1, 5),
            Sp("Corner", "chcorner", 0, 3, 1, 1, {"Top Left", "Top Right", "Bottom Left", "Bottom Right"}),
        }},
        {"anchors", {
            Sp("Anchor Interval", "anchorint", 10, 600, 10, 60),
            Sp("Position Tolerance", "anchortol", 0, 20, 0.5, 1.5),
            TK("anchor_vel", "Restore Velocity", true),
            TK("anchor_rot", "Restore Rotation"),
        }},
        {"labelstyle", {
            Sp("Label Size", "lblscale", 0.5, 2, 0.1, 1),
            Sp("Label Color", "lblcol", 0, 8, 1, 8, COLNAMES),
            Sp("Label Opacity", "lblop", 0.1, 1, 0.1, 0.8),
        }},
    };
    static std::vector<Feat> const empty = { Nt("No extra options for this feature yet.") };
    auto it = m.find(id);
    return it == m.end() ? empty : it->second;
}

static bool isImpl(std::string const& k) {
    static std::set<std::string> const s = {
        "speedhack", "safemode", "autosafemode", "noclip", "jumphack", "hideplayer", "nodeatheffect",
        "unlockicons", "autoldm", "autopracticemode", "showhitboxes", "hitboxesondeath",
        "accuratepercentage", "freezeattempts", "notrail", "holdinfo", "useanchors", "showanchors", "showlabels", "cheatindicator", "togglenotifications",
        "pitchshift", "audiospeed", "syncwithspeedhack", "confirmrestart", "confirmfullrestart",
        "confirmpracticemode", "hidelevelcompletevfx", "layoutmode", "noobjectglow", "noparticles",
        "novehicleparticles", "norobotfire", "noghosttrail", "nowavetrailbehind", "solidwavetrail",
        "autokill", "autoclicker", "respawndelay", "instantcomplete", "physicsbypass",
        "confirmquit", "deathpercentpopup", "mutelevelaudioondeath"
    };
    return s.count(k) > 0;
}

static std::string fmtVal(Feat const& r, double v) {
    if (!r.names.empty()) {
        int i = std::clamp((int)std::lround(v), 0, (int)r.names.size() - 1);
        return r.names[i];
    }
    return r.step >= 1.0 ? fmt::format("{:.0f}", v) : fmt::format("{:.1f}", v);
}


// ───────────────────────── bot (macro + anchors) ─────────────────────────
struct PState { float x = 0, y = 0, rot = 0; double yv = 0; bool up = false; };
struct BotAnchor { int frame = 0; bool dual = false; PState p1, p2; };
struct BInput { int frame = 0; bool down = false; int button = 1; bool p1 = true; };
static std::vector<BInput> s_inputs;
static std::vector<BotAnchor> s_anchors;
static int s_fixes = 0;
static bool s_injecting = false;

static int botFrame(GJBaseGameLayer* gl) { return (int)gl->m_gameState.m_currentProgress; }
static PState snapP(PlayerObject* p) {
    PState s;
    if (!p) return s;
    s.x = p->getPositionX(); s.y = p->getPositionY(); s.rot = p->getRotation();
    s.yv = p->m_yVelocity; s.up = p->m_isUpsideDown;
    return s;
}
static void applyP(PlayerObject* p, PState const& s) {
    if (!p) return;
    p->setPosition(CCPoint(s.x, s.y));
    if (optd("anchor_vel", true)) p->m_yVelocity = s.yv;
    if (opt("anchor_rot")) p->setRotation(s.rot);
}
static bool offPath(PlayerObject* p, PState const& s) {
    if (!p) return false;
    float dx = p->getPositionX() - s.x, dy = p->getPositionY() - s.y;
    if (std::sqrt(dx * dx + dy * dy) > (float)getVal("anchortol", 1.5)) return true;
    return optd("anchor_vel", true) && std::fabs(p->m_yVelocity - s.yv) > 0.5;
}
static void addAnchor(PlayLayer* pl) {
    int fr = botFrame(pl);
    BotAnchor a;
    a.frame = fr;
    a.p1 = snapP(pl->m_player1);
    a.dual = pl->m_gameState.m_isDualMode;
    if (a.dual) a.p2 = snapP(pl->m_player2);
    while (!s_anchors.empty() && s_anchors.back().frame >= fr) s_anchors.pop_back();
    s_anchors.push_back(a);
}
// runs at the start of every physics step
static void botStep(GJBaseGameLayer* gl) {
    int mode = (int)getVal("botmode", 0);
    if (mode == 0) return;
    auto pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != gl) return;
    if (pl->m_player1 && pl->m_player1->m_isDead) return;
    int fr = botFrame(gl);
    bool useA = optd("useanchors", true);
    if (mode == 1) {
        int iv = std::max(1, (int)getVal("anchorint", 60.0));
        if (useA && fr > 0 && fr % iv == 0) addAnchor(pl);
        return;
    }
    // replay: inject the recorded inputs for this frame
    auto it = std::lower_bound(s_inputs.begin(), s_inputs.end(), fr,
                               [](BInput const& a, int f) { return a.frame < f; });
    s_injecting = true;
    for (; it != s_inputs.end() && it->frame == fr; ++it)
        gl->handleButton(it->down, it->button, it->p1);
    s_injecting = false;
    // then put the player back on the path if it drifted
    if (useA) {
        auto ai = std::lower_bound(s_anchors.begin(), s_anchors.end(), fr,
                                   [](BotAnchor const& a, int f) { return a.frame < f; });
        if (ai != s_anchors.end() && ai->frame == fr) {
            if (offPath(pl->m_player1, ai->p1)) { applyP(pl->m_player1, ai->p1); s_fixes++; }
            if (ai->dual && pl->m_gameState.m_isDualMode && offPath(pl->m_player2, ai->p2)) {
                applyP(pl->m_player2, ai->p2); s_fixes++;
            }
        }
    }
}

static std::filesystem::path macroPath() { return Mod::get()->getSaveDir() / "macro.txt"; }
static void writeP(std::ostream& o, PState const& p) {
    o << p.x << ' ' << p.y << ' ' << p.rot << ' ' << p.yv << ' ' << (p.up ? 1 : 0) << ' ';
}
static void readP(std::istream& i, PState& p) {
    int u = 0;
    i >> p.x >> p.y >> p.rot >> p.yv >> u;
    p.up = u != 0;
}
static bool saveMacro() {
    std::ofstream o(macroPath());
    if (!o) return false;
    for (auto const& a : s_inputs) o << "I " << a.frame << ' ' << (a.down ? 1 : 0) << ' ' << a.button << ' ' << (a.p1 ? 1 : 0) << '\n';
    for (auto const& a : s_anchors) {
        o << "A " << a.frame << ' ' << (a.dual ? 1 : 0) << ' ';
        writeP(o, a.p1); writeP(o, a.p2);
        o << '\n';
    }
    return true;
}
static bool loadMacro() {
    std::ifstream i(macroPath());
    if (!i) return false;
    s_inputs.clear(); s_anchors.clear();
    std::string tag;
    while (i >> tag) {
        if (tag == "I") {
            BInput b; int d = 0, p = 1;
            i >> b.frame >> d >> b.button >> p;
            b.down = d != 0; b.p1 = p != 0;
            s_inputs.push_back(b);
        } else if (tag == "A") {
            BotAnchor a; int d = 0;
            i >> a.frame >> d;
            a.dual = d != 0;
            readP(i, a.p1); readP(i, a.p2);
            s_anchors.push_back(a);
        } else break;
    }
    return true;
}

// ───────────────────────── descriptions (shown on hold) ─────────────────────────
static std::vector<std::pair<char const*, char const*>> const DESC_LIST = {
    {"Physics Bypass", "Sets the physics ticks per second (default 240). Best effort and untested: Windows and Android only, nothing happens on macOS. Counts as a cheat for Safe Mode."},
    {"Speedhack", "Changes the game speed while you are in a level."},
    {"Pitch Shift", "Changes the pitch of the level music (1 = normal, 2 = higher/faster, 0.5 = lower/slower). Also changes tempo."},
    {"Audio Speed", "Changes how fast the level music plays (1 = normal, 2 = double speed)."},
    {"Sync with Speedhack", "Makes the level music speed follow your Speedhack value so music and gameplay stay in time."},
    {"Auto Safe Mode", "Turns Safe Mode on automatically once a cheat is used, so cheated runs never save progress."},
    {"Click Between Steps", "Registers clicks between physics steps. (Not available yet.)"},
    {"Click On Steps", "Only registers clicks on physics steps. (Not available yet.)"},
    {"Compact Editor Levels", "Smaller level rows in the editor list. (Not available yet.)"},
    {"Compact Profile Comments", "Smaller comment rows on profiles. (Not available yet.)"},
    {"No Short Numbers", "Shows full numbers instead of 1.2K style. (Not available yet.)"},
    {"Safe Mode", "Completions and progress are never saved while this is on."},
    {"Show Taps", "Shows where you tap on screen. (Not available yet.)"},
    {"0% Practice Completion", "Allows completing practice mode from 0%. (Not available yet.)"},
    {"Accurate Percentage", "Shows decimals on the percentage label."},
    {"All Modes in Platformer", "Lets every gamemode work in platformer levels. (Not available yet.)"},
    {"All Passable", "Makes every solid block passable. (Not available yet.)"},
    {"Always Show Coins", "Shows coins even before they are unlocked. (Not available yet.)"},
    {"Auto LDM", "Turns on Low Detail Mode automatically."},
    {"Auto Pickup Coins", "Collects coins automatically. (Not available yet.)"},
    {"Auto Practice Mode", "Enters practice mode as soon as a level starts."},
    {"Auto Song Download", "Downloads missing songs automatically. (Not available yet.)"},
    {"Checkpoint Delay", "Delays practice checkpoint placement. (Not available yet.)"},
    {"Confirm Full Restart", "Asks you to confirm before Restart From Start in the pause menu."},
    {"Confirm Practice Mode", "Asks you to confirm before entering practice mode from the pause menu."},
    {"Confirm Restart", "Asks you to confirm before Restart in the pause menu."},
    {"Force Platformer", "Plays any level as a platformer. (Not available yet.)"},
    {"Freeze Attempts", "The attempt counter stops going up."},
    {"Hide Level Complete VFX", "Hides the Level Complete text and its end-of-level effects."},
    {"Hide Pause Menu", "Hides the pause menu. (Not available yet.)"},
    {"Hitbox Multiplier", "Scales the size of hitboxes. (Not available yet.)"},
    {"Instant Mirror", "Mirror portals flip instantly. (Not available yet.)"},
    {"Layout Mode", "Hides decoration objects so only the gameplay layout is visible."},
    {"Legacy Pulse", "Old style pulse effects. (Not available yet.)"},
    {"Legacy Reverse Physics", "Old reverse physics behaviour. (Not available yet.)"},
    {"Legacy Trail", "Old style player trail. (Not available yet.)"},
    {"Match Percent/Time Fonts", "Uses the same font for percent and time labels. (Not available yet.)"},
    {"Mute Level SFX", "Mutes sound effects inside levels. (Not available yet.)"},
    {"No End Shake", "Removes the screen shake at the end. (Not available yet.)"},
    {"No Mirror", "Mirror portals do nothing. (Not available yet.)"},
    {"No Music Fade Out", "Music does not fade out. (Not available yet.)"},
    {"No Object Glow", "Hides the glow around objects."},
    {"No Particles", "Hides player and vehicle particle effects."},
    {"No Portal Lightning", "Removes portal lightning. (Not available yet.)"},
    {"No Shaders", "Turns off shader effects. (Not available yet.)"},
    {"Pause Countdown", "Counts down before resuming. (Not available yet.)"},
    {"Pause During Complete", "Lets you pause during the complete animation. (Not available yet.)"},
    {"Practice Coins", "Lets practice mode collect coins. (Not available yet.)"},
    {"Practice Fix", "Fixes practice mode checkpoint bugs. (Not available yet.)"},
    {"Random Seed", "Fixes the random trigger seed. (Not available yet.)"},
    {"Show Hitboxes", "Draws hitboxes for hazards, solids and the player."},
    {"Hitboxes On Death", "Freezes the hitboxes on screen at the moment you die."},
    {"Show Triggers", "Shows trigger objects. (Not available yet.)"},
    {"Smart StartPos", "Fixes start positions. (Not available yet.)"},
    {"StartPos Switcher", "Switch between start positions. (Not available yet.)"},
    {"Stop Triggers on Death", "Stops triggers when you die. (Not available yet.)"},
    {"Allow Low Volume", "Lets volume go lower than normal. (Not available yet.)"},
    {"Character Filter Bypass", "Bypasses the text filter. (Not available yet.)"},
    {"Character Limit Bypass", "Bypasses text length limits. (Not available yet.)"},
    {"Checkpoint Limit", "Removes the checkpoint limit. (Not available yet.)"},
    {"Quick Chests", "No waiting on chests. (Not available yet.)"},
    {"Instant Complete", "Completes the level a few frames after it starts. Counts as a cheat for Safe Mode."},
    {"Level Kick", "Stops the game kicking you out of levels. (Not available yet.)"},
    {"Menu Gameplay", "Lets you play on the main menu. (Not available yet.)"},
    {"Music Customizer", "Pick custom music. (Not available yet.)"},
    {"Pause Buffering", "Buffers inputs while paused. (Not available yet.)"},
    {"Practice Music Sync", "Syncs music in practice mode. (Not available yet.)"},
    {"Show Spam Comments", "Shows hidden spam comments. (Not available yet.)"},
    {"Swift Click", "Faster menu clicking. (Not available yet.)"},
    {"Unlock Icons", "Unlocks every icon and colour."},
    {"Unlock Main Levels", "Unlocks main levels. (Not available yet.)"},
    {"Unlock Paths", "Unlocks paths. (Not available yet.)"},
    {"Unlock Shops", "Unlocks shops. (Not available yet.)"},
    {"Unlock Vaults", "Unlocks vaults. (Not available yet.)"},
    {"Always Show Trail", "Always shows the player trail. (Not available yet.)"},
    {"AutoClicker", "Taps player 1's button automatically at the clicks per second you enter. Counts as a cheat for Safe Mode."},
    {"Auto Kill", "Kills you when the level reaches the percentage you enter (under 100). Useful for practising a section."},
    {"Ball Rotation Bug", "Brings back the ball rotation bug. (Not available yet.)"},
    {"Custom Wave Trail", "Custom wave trail style. (Not available yet.)"},
    {"Force Ghost Trail", "Always shows the ghost trail. (Not available yet.)"},
    {"Frame Stepper", "Step the game one frame at a time. (Not available yet.)"},
    {"Hide Player", "Makes the player invisible."},
    {"Jump Hack", "Lets you jump at any time, even in the air."},
    {"Mute Level Audio on Death", "Pauses the level music when you die and resumes it when you respawn."},
    {"Mute Rewards SFX on Death", "Mutes reward sounds on death. (Not available yet.)"},
    {"No Death Effect", "Skips the death explosion."},
    {"No Ghost Trail", "Hides the ghost trail effect."},
    {"No Hide Trigger", "Ignores hide player triggers. (Not available yet.)"},
    {"No Platformer Jump Anim", "Removes the platformer jump animation. (Not available yet.)"},
    {"No Respawn Flash", "Removes the flash on respawn. (Not available yet.)"},
    {"No Robot Fire", "Hides the fire coming out of the robot's feet."},
    {"No Spider Dash", "Removes the spider dash effect. (Not available yet.)"},
    {"No Trail", "Hides the player's motion trail."},
    {"No Vehicle Particles", "Hides the ground and click particles on ship, UFO and other vehicles."},
    {"No Wave Trail Behind", "Hides the trail the wave leaves behind."},
    {"Noclip", "You do not die when hitting hazards or solids."},
    {"Shipcopter", "Ship flies like a copter. (Not available yet.)"},
    {"Show Trajectory", "Shows where you will go. (Not available yet.)"},
    {"Solid Wave Trail", "Makes the wave trail a solid line."},
    {"Wave Trail On Death", "Keeps the wave trail after death. (Not available yet.)"},
    {"Respawn Delay", "Seconds to wait before respawning after death. Normal is 1; lower is faster, higher is slower."},
    {"Bot Mode", "Off, Record (saves your inputs and anchors) or Replay (plays them back)."},
    {"Use Anchors", "While recording, saves your position and velocity every few frames. While replaying, snaps you back onto the path if you drift off it."},
    {"Show Anchors", "Draws a dot at every saved anchor point."},
    {"Add Anchor Now", "Saves an anchor at your current position and velocity right now (inside a level)."},
    {"Clear Macro", "Deletes the recorded inputs and anchors from memory."},
    {"Save Macro", "Saves the recorded macro to a file."},
    {"Load Macro", "Loads the saved macro from the file."},
    {"Anchor Interval", "How many frames between automatic anchors while recording."},
    {"Position Tolerance", "How far off path you can be before an anchor moves you back."},
    {"Restore Velocity", "Anchors also restore vertical velocity."},
    {"Restore Rotation", "Anchors also restore the player rotation."},
    {"Accent", "Colour of the selected tab and the dot."},
    {"Menu Opacity", "How see-through the menu is."},
    {"Dot Size", "Size of the dot or of the cheat indicator."},
    {"Dot Opacity", "How see-through the dot is."},
    {"Toggle Notifications", "Shows a pop up whenever you flip a toggle."},
    {"Hold For Descriptions", "Hold your finger on a label to see what it does."},
    {"Hold Time", "How long you hold before the description shows."},
    {"Reset Dot Position", "Moves the dot back to the top left."},
    {"Reset Style", "Resets accent, opacity and dot settings."},
    {"Restart Level", "Restarts the level you are in. Its keyboard key can be changed with the key settings below."},
    {"Toggle Practice Mode", "Turns practice mode on or off. Its keyboard key can be changed with the key settings below."},
    {"Respawn From Start", "Restarts from the beginning, ignoring checkpoints."},
    {"Toggle Hitboxes", "Turns Show Hitboxes on or off. Its keyboard key can be changed with the key settings below."},
    {"Toggle Noclip", "Turns Noclip on or off. Its keyboard key can be changed with the key settings below."},
    {"Show Labels", "Master switch for all HUD labels."},
    {"Show in Editor", "Show labels in the editor. (Not available yet.)"},
    {"Cheat Indicator", "A dot that turns red once a cheat has been used this level."},
    {"Noclip P1 Only", "Noclip only applies to player 1."},
    {"Noclip P2 Only", "Noclip only applies to player 2."},
    {"Noclip Flash", "Flashes the screen when noclip saves you."},
    {"Flash Color", "Colour of the noclip flash."},
    {"Flash Opacity", "Strength of the noclip flash."},
    {"Only In Practice", "Only works in practice mode."},
    {"Pass Hazards Only", "Noclip only ignores hazards."},
    {"Pass Solids Only", "Noclip only ignores solids."},
    {"Red Hitbox On Block", "Player hitbox turns red when noclip blocks a death."},
    {"Start At %", "Noclip turns on at this percentage."},
    {"Stop At %", "Noclip turns off at this percentage."},
    {"Min Accuracy %", "Kills you if noclip accuracy drops below this."},
    {"Max NC Deaths", "Kills you after this many blocked deaths."},
    {"Fill Hitboxes", "Fills hitboxes with a faint colour."},
    {"Hitbox Trail", "Draws a trail of your player hitbox."},
    {"Trail Length", "How long the hitbox trail is."},
    {"Hazard Color", "Colour of hazard hitboxes."},
    {"Solid Color", "Colour of solid hitboxes."},
    {"Player Color", "Colour of the player hitbox."},
    {"Trail Color", "Colour of the hitbox trail."},
    {"Line Thickness", "Thickness of hitbox lines."},
    {"Label Size", "Size of the HUD labels."},
    {"Label Color", "Colour of the HUD labels."},
    {"Label Opacity", "How see-through the HUD labels are."},
    {"Trigger On Noclip", "Auto Safe Mode turns on when you use noclip."},
    {"Trigger On Jump Hack", "Auto Safe Mode turns on when you use jump hack."},
    {"Trigger On Speedhack", "Auto Safe Mode turns on when you change speed."},
    {"Trigger On Bot Replay", "Auto Safe Mode turns on when you replay a macro."},
    {"Warn On Level Complete", "Shows a warning when a Safe Mode completion is not saved."},
    {"Decimals", "How many decimals the percentage shows."},
    {"Player 1 Only", "Only player 1 gets jump hack."},
    {"Keep Player 1 Visible", "Player 1 stays visible when hiding the player."},
    {"Keep Player 2 Visible", "Player 2 stays visible when hiding the player."},
    {"Also Player 2", "No Trail also applies to player 2."},
    {"Include Colors", "Unlock Icons also unlocks colours."},
    {"Corner", "Which corner the cheat indicator sits in."},
    // hud labels
    {"FPS", "Shows your frames per second."},
    {"Testmode", "Shows Safe Mode or Testmode when progress will not save."},
    {"Run From", "Percentage this run started from."},
    {"Attempt", "Current attempt number."},
    {"Percentage", "Current level percentage."},
    {"Level Time", "Time spent in this attempt."},
    {"Best Run", "Best percentage this session."},
    {"Clock", "The time of day."},
    {"CPS", "Clicks per second."},
    {"Noclip Accuracy", "How much of the run noclip did not have to save you."},
    {"Noclip Deaths", "How many deaths noclip blocked this attempt."},
    {"Jumps", "Clicks this attempt."},
    {"Level Name", "Name of the level."},
    {"Bot", "Bot mode, macro size and how many times anchors fixed you."},
    {"Restart Key", "Keyboard key that restarts the level (Windows). Off disables it."},
    {"Practice Key", "Keyboard key that toggles practice mode (Windows). Off disables it."},
    {"Full Restart Key", "Keyboard key that restarts from the very start, ignoring checkpoints (Windows). Off disables it."},
    {"Hitbox Key", "Keyboard key that toggles Show Hitboxes (Windows). Off disables it."},
    {"Noclip Key", "Keyboard key that toggles Noclip (Windows). Off disables it."},
    {"Frame", "Current physics frame of this attempt."},
    {"Position", "Player 1's X and Y position."},
    {"Y Velocity", "Player 1's vertical speed."},
    {"Speed", "The speedhack multiplier currently in use."},
    {"TPS", "Physics ticks per second currently in use."},
    {"Confirm Quit", "Asks you to confirm before quitting the level from the pause menu."},
    {"Death Percent Popup", "Shows a small popup with the percentage you died at."},
};
static std::string descByLabel(std::string const& label) {
    static std::map<std::string, std::string> m = [] {
        std::map<std::string, std::string> r;
        for (auto const& kv : DESC_LIST) r[autoKey(kv.first)] = kv.second;
        return r;
    }();
    auto it = m.find(autoKey(label));
    return it == m.end() ? std::string() : it->second;
}
static std::string descOf(Feat const& f) {
    if (!f.info.empty()) return f.info;
    auto d = descByLabel(f.label);
    return d.empty() ? std::string("No description yet.") : d;
}

// holding a finger on a label shows its description
class HoldLabel : public CCNode, public CCTouchDelegate {
public:
    CCNode* m_target = nullptr;
    std::string m_title, m_text;
    bool m_down = false, m_fired = false;
    float m_t = 0.f;
    CCPoint m_start;

    static HoldLabel* create(CCNode* target, std::string title, std::string text) {
        auto r = new HoldLabel();
        if (r && r->init()) {
            r->m_target = target; r->m_title = std::move(title); r->m_text = std::move(text);
            r->scheduleUpdate();
            r->autorelease();
            return r;
        }
        CC_SAFE_DELETE(r);
        return nullptr;
    }
    void onEnter() override {
        CCNode::onEnter();
        CCTouchDispatcher::get()->addTargetedDelegate(this, -130, false); // doesn't swallow: scrolling still works
    }
    void onExit() override {
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }
    bool ccTouchBegan(CCTouch* t, CCEvent*) override;
    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        if (m_down && ccpDistance(t->getLocation(), m_start) > 10.f) m_down = false;
    }
    void ccTouchEnded(CCTouch*, CCEvent*) override { m_down = false; }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { m_down = false; }
    void update(float dt) override {
        if (!m_down || m_fired) return;
        m_t += dt;
        if (m_t >= (float)getVal("holdtime", 0.5)) {
            m_fired = true;
            Notification::create(m_title + ": " + m_text, NotificationIcon::Info, 3.f)->show();
        }
    }
};

// ───────────────────────── widgets ─────────────────────────
static CCScale9Sprite* makeBox(float w, float h, ccColor3B col, GLubyte op = 255) {
    auto s = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    s->setContentSize({w * 3.f, h * 3.f});
    s->setScale(1.f / 3.f);
    s->setColor(col);
    s->setOpacity(op);
    return s;
}

static CCNode* makePill(char const* text, float w, float h, float fs, ccColor3B col, GLubyte op,
                        ccColor3B tc = {255, 255, 255}) {
    auto n = CCNode::create();
    n->setContentSize({w, h});
    auto b = makeBox(w, h, col, op);
    b->setPosition({w / 2, h / 2});
    b->setTag(78);
    n->addChild(b);
    auto l = CCLabelBMFont::create(text, "bigFont.fnt");
    l->setScale(fs);
    l->limitLabelWidth(w - 10.f, fs, 0.1f);
    l->setPosition({w / 2, h / 2});
    l->setColor(tc);
    l->setTag(77);
    n->addChild(l);
    return n;
}

static CCNode* iconInfo() {
    auto n = CCNode::create();
    n->setContentSize({14, 14});
    auto d = CCDrawNode::create();
    d->drawDot({7, 7}, 7, {1.f, 1.f, 1.f, 1.f});
    n->addChild(d);
    auto l = CCLabelBMFont::create("i", "bigFont.fnt");
    l->setScale(0.38f);
    l->setColor(C_PANEL);
    l->setPosition({7, 7.5f});
    n->addChild(l);
    return n;
}
static CCNode* iconGear() {
    auto n = CCNode::create();
    n->setContentSize({22, 22});
    auto b = makeBox(22, 22, C_BTN);
    b->setPosition({11, 11});
    n->addChild(b);
    auto d = CCDrawNode::create();
    CCPoint c {11.f, 11.f};
    float r = 7.f;
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159265f / 4.f;
        d->drawSegment({c.x + std::cos(a) * r * 0.6f, c.y + std::sin(a) * r * 0.6f},
                       {c.x + std::cos(a) * r * 1.05f, c.y + std::sin(a) * r * 1.05f},
                       r * 0.2f, {1.f, 1.f, 1.f, 1.f});
    }
    d->drawDot(c, r * 0.78f, {1.f, 1.f, 1.f, 1.f});
    d->drawDot(c, r * 0.34f, f4(C_BTN, 1.f));
    n->addChild(d);
    return n;
}
static CCNode* iconTrash() {
    auto n = CCNode::create();
    n->setContentSize({22, 22});
    auto b = makeBox(22, 22, C_BTN);
    b->setPosition({11, 11});
    n->addChild(b);
    auto d = CCDrawNode::create();
    ccColor4F w {1.f, 1.f, 1.f, 1.f};
    CCPoint body[4] = {{6.5f, 4.f}, {15.5f, 4.f}, {14.5f, 14.f}, {7.5f, 14.f}};
    d->drawPolygon(body, 4, w, 0.f, {0, 0, 0, 0});
    d->drawSegment({5.f, 15.5f}, {17.f, 15.5f}, 0.9f, w);
    d->drawSegment({9.f, 15.5f}, {9.f, 17.5f}, 0.7f, w);
    d->drawSegment({13.f, 15.5f}, {13.f, 17.5f}, 0.7f, w);
    d->drawSegment({9.f, 17.5f}, {13.f, 17.5f}, 0.7f, w);
    ccColor4F g = f4(C_BTN, 1.f);
    d->drawSegment({9.f, 6.f}, {9.f, 12.f}, 0.6f, g);
    d->drawSegment({11.f, 6.f}, {11.f, 12.f}, 0.6f, g);
    d->drawSegment({13.f, 6.f}, {13.f, 12.f}, 0.6f, g);
    n->addChild(d);
    return n;
}
static CCNode* iconArrow(bool up) {
    auto n = CCNode::create();
    n->setContentSize({22, 22});
    auto d = CCDrawNode::create();
    CCPoint t[3];
    if (up) { t[0] = CCPoint(3.f, 5.f); t[1] = CCPoint(19.f, 5.f); t[2] = CCPoint(11.f, 17.f); }
    else    { t[0] = CCPoint(3.f, 17.f); t[1] = CCPoint(19.f, 17.f); t[2] = CCPoint(11.f, 5.f); }
    d->drawPolygon(t, 3, {0.88f, 0.9f, 1.f, 1.f}, 0.f, {0, 0, 0, 0});
    n->addChild(d);
    return n;
}

// ───────────────────────── draggable dot ─────────────────────────
class DotNode : public CCNode, public CCTouchDelegate {
public:
    std::function<void()> m_onTap;
    CCDrawNode* m_draw = nullptr;
    bool m_moved = false;
    CCPoint m_start, m_off;
    float m_r = 18.f;

    static DotNode* create(std::function<void()> tap) {
        auto r = new DotNode();
        if (r && r->init()) { r->m_onTap = std::move(tap); r->setup(); r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    void setup() {
        m_draw = CCDrawNode::create();
        this->addChild(m_draw);
        rebuild();
        resetPos(false);
        CCTouchDispatcher::get()->addTargetedDelegate(this, -600, true);
    }

    ~DotNode() override { CCTouchDispatcher::get()->removeDelegate(this); }

    void rebuild() {
        m_r = (float)getVal("dotsize", 18.0);
        float a = (float)getVal("dotop", 0.6);
        auto ac = accentColor();
        m_draw->clear();
        m_draw->drawDot({0, 0}, m_r, f4(ac, a * 0.8f));
        m_draw->drawDot({0, 0}, m_r - 2.f, {0.04f, 0.04f, 0.08f, a});
        float s = m_r * 0.45f, c = m_r * 0.2f;
        CCPoint sq[4] = {{-s, -s}, {s, -s}, {s, s}, {-s, s}};
        m_draw->drawPolygon(sq, 4, {0, 0, 0, 0}, 1.3f, {1.f, 0.24f, 0.24f, a});
        CCPoint in[4] = {{-c, -c}, {c, -c}, {c, c}, {-c, c}};
        m_draw->drawPolygon(in, 4, f4(ac, a), 0.f, {0, 0, 0, 0});
    }

    void resetPos(bool clear) {
        auto win = CCDirector::get()->getWinSize();
        if (clear) { setVal("dotx", 30.0); setVal("doty", win.height - 30.0); }
        this->setPosition({(float)getVal("dotx", 30.0), (float)getVal("doty", win.height - 30.0)});
    }

    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!this->getParent() || !this->isVisible()) return false;
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        if (ccpDistance(p, this->getPosition()) > m_r + 8.f) return false;
        m_moved = false;
        m_start = p;
        m_off = this->getPosition() - p;
        return true;
    }
    void ccTouchMoved(CCTouch* t, CCEvent*) override {
        auto p = this->getParent()->convertTouchToNodeSpace(t);
        if (!m_moved && ccpDistance(p, m_start) > 6.f) m_moved = true;
        if (m_moved) {
            auto win = CCDirector::get()->getWinSize();
            CCPoint np = p + m_off;
            np.x = std::clamp(np.x, m_r, win.width - m_r);
            np.y = std::clamp(np.y, m_r, win.height - m_r);
            this->setPosition(np);
        }
    }
    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (m_moved) {
            setVal("dotx", this->getPositionX());
            setVal("doty", this->getPositionY());
        } else if (m_onTap) m_onTap();
    }
    void ccTouchCancelled(CCTouch*, CCEvent*) override {}
};

// ───────────────────────── menu ─────────────────────────
class ModMenu : public CCNode, public CCTouchDelegate {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    CCNode* m_bg = nullptr;
    CCNode* m_content = nullptr;
    DotNode* m_dot = nullptr;
    std::vector<CCNode*> m_tabPills;
    std::vector<CCLabelBMFont*> m_valLabels;
    std::vector<Feat> m_feats;     // features currently on screen (button tags index this)
    std::string m_sub, m_subTitle; // open gear sub-page
    int m_tab = 0;
    int m_preset = 0;
    bool m_open = false;
    float m_fps = 60.f;
    CCSize m_ps {476.f, 275.f};
    float m_sbW = 114.f;
    float m_cx = 0, m_cy = 0, m_cw = 0, m_ch = 0; // scroll area (panel space)

    static ModMenu* create() {
        auto r = new ModMenu();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    ~ModMenu() override { CCTouchDispatcher::get()->removeDelegate(this); }

    bool init() override {
        if (!CCNode::init()) return false;
        auto win = CCDirector::get()->getWinSize();
        m_ps = CCSize(std::floor(win.width * 0.67f), std::floor(win.height * 0.86f));
        auto ps = m_ps;
        m_sbW = std::floor(ps.width * 0.24f);

        float contentX = 8.f + m_sbW + 8.f;
        m_cx = contentX + 5.f;
        m_cy = 12.f;
        m_cw = ps.width - contentX - 8.f - 10.f;
        m_ch = ps.height - 24.f;

        m_panel = CCNode::create();
        m_panel->setContentSize(ps);
        m_panel->setAnchorPoint({0.5f, 0.5f});
        m_panel->setPosition({win.width / 2, win.height / 2});
        m_panel->setScale(0.01f);
        m_panel->setVisible(false);
        this->addChild(m_panel);

        m_bg = CCNode::create();
        m_panel->addChild(m_bg);

        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({0, 0});
        m_panel->addChild(tabMenu, 2);
        auto& TB = tabs();
        float pitch = (ps.height - 16.f) / (float)TB.size();
        for (int i = 0; i < (int)TB.size(); i++) {
            auto pill = makePill(TB[i].name, m_sbW - 8.f, pitch - 4.f, 0.42f, C_BTN, 255);
            m_tabPills.push_back(pill);
            auto item = CCMenuItemSpriteExtra::create(pill, this, menu_selector(ModMenu::onTab));
            item->setTag(i);
            item->setPosition({8.f + m_sbW / 2, ps.height - 8.f - pitch * (i + 0.5f)});
            tabMenu->addChild(item);
        }

        m_content = CCNode::create();
        m_panel->addChild(m_content, 1);

        m_dot = DotNode::create([this]() { this->toggle(); });
        if (m_dot) this->addChild(m_dot, 5);

        applyStyle();
        refresh();
        this->scheduleUpdate();
        return true;
    }

    // ── touch blocker so taps on the panel don't reach the game ──
    bool ccTouchBegan(CCTouch* t, CCEvent*) override {
        if (!m_open || !m_panel) return false;
        auto p = m_panel->convertTouchToNodeSpace(t);
        if (!CCRect(0, 0, m_ps.width, m_ps.height).containsPoint(p)) return false;
        if (CCRect(m_cx, m_cy, m_cw, m_ch).containsPoint(p)) return false; // scroll layer handles it
        return true;
    }
    void ccTouchMoved(CCTouch*, CCEvent*) override {}
    void ccTouchEnded(CCTouch*, CCEvent*) override {}
    void ccTouchCancelled(CCTouch*, CCEvent*) override {}

    // ── styling ──
    void recolorTabs() {
        auto ac = accentColor();
        for (int i = 0; i < (int)m_tabPills.size(); i++) {
            bool sel = i == m_tab;
            if (auto b = static_cast<CCScale9Sprite*>(m_tabPills[i]->getChildByTag(78)))
                b->setColor(sel ? ac : C_BTN);
            if (auto l = static_cast<CCLabelBMFont*>(m_tabPills[i]->getChildByTag(77)))
                l->setColor(sel ? C_DARK : C_WHITE);
        }
    }

    void applyLight() {
        recolorTabs();
        if (m_dot) m_dot->rebuild();
    }

    void applyStyle() {
        m_bg->removeAllChildren();
        float op = (float)getVal("menuop", 0.95);
        GLubyte o = (GLubyte)(255.f * op);
        auto ps = m_ps;

        auto p = makeBox(ps.width, ps.height, C_PANEL, o);
        p->setPosition({ps.width / 2, ps.height / 2});
        m_bg->addChild(p);

        auto sb = makeBox(m_sbW, ps.height - 16.f, C_SIDE, o);
        sb->setPosition({8.f + m_sbW / 2, ps.height / 2});
        m_bg->addChild(sb);

        float bw = ps.width - (8.f + m_sbW + 8.f) - 8.f;
        auto cb = makeBox(bw, ps.height - 16.f, C_SIDE, o);
        cb->setPosition({8.f + m_sbW + 8.f + bw / 2, ps.height / 2});
        m_bg->addChild(cb);
        applyLight();
    }

    // ── small builders ──
    void deferRefresh() {
        Loader::get()->queueInMainThread([this]() { this->refresh(); });
    }

    void addCheck(CCMenu* menu, float x, float y, bool on, int tag, SEL_MenuHandler cb) {
        auto mk = [](bool state) {
            auto n = CCNode::create();
            n->setContentSize({20, 20});
            auto b = makeBox(20, 20, C_BTN);
            b->setPosition({10, 10});
            n->addChild(b);
            if (state) {
                auto d = CCDrawNode::create();
                ccColor4F w {1.f, 1.f, 1.f, 1.f};
                d->drawSegment({4.f, 10.f}, {8.5f, 5.5f}, 1.4f, w);
                d->drawSegment({8.5f, 5.5f}, {16.f, 14.5f}, 1.4f, w);
                n->addChild(d);
            }
            return n;
        };
        auto t = CCMenuItemToggler::create(mk(false), mk(true), this, cb);
        t->setTag(tag);
        t->toggle(on);
        t->setPosition({x, y});
        menu->addChild(t);
    }
    void addIcon(CCMenu* menu, CCNode* icon, float x, float y, int tag, SEL_MenuHandler cb) {
        auto b = CCMenuItemSpriteExtra::create(icon, this, cb);
        b->setTag(tag);
        b->setPosition({x, y});
        menu->addChild(b);
    }
    CCLabelBMFont* addHold(CCNode* cl, CCLabelBMFont* l, std::string const& title, std::string const& text) {
        if (l) cl->addChild(HoldLabel::create(l, title, text));
        return l;
    }
    bool inScroll(CCTouch* t) {
        if (!m_panel) return false;
        auto p = m_panel->convertTouchToNodeSpace(t);
        return CCRect(m_cx, m_cy, m_cw, m_ch).containsPoint(p);
    }
    CCLabelBMFont* addText(CCNode* parent, std::string const& s, float x, float y, float maxW,
                           float sc = 0.4f, bool center = false) {
        auto l = CCLabelBMFont::create(s.c_str(), "bigFont.fnt");
        l->setScale(sc);
        l->limitLabelWidth(maxW, sc, 0.12f);
        l->setAnchorPoint({center ? 0.5f : 0.f, 0.5f});
        l->setPosition({x, y});
        parent->addChild(l);
        return l;
    }

    CCNode* beginScroll(int rows, CCMenu*& menu, float& H) {
        H = std::max(m_ch, rows * ROWH + 8.f);
        auto sl = ScrollLayer::create({m_cw, m_ch});
        sl->setPosition({m_cx, m_cy});
        m_content->addChild(sl);
        auto cl = sl->m_contentLayer;
        cl->setContentSize({m_cw, H});
        cl->setPositionY(m_ch - H); // start scrolled to the top
        menu = CCMenu::create();
        menu->setPosition({0, 0});
        cl->addChild(menu, 5);
        return cl;
    }

    int countRows(std::vector<Feat> const& fs) {
        int row = 0, col = 0;
        for (auto const& f : fs) {
            if (f.k == Feat::Toggle) { if (++col == 2) { col = 0; row++; } }
            else { if (col) { row++; col = 0; } row++; }
        }
        if (col) row++;
        return row;
    }

    // lays out features top-down; tags index m_feats
    int placeFeats(std::vector<Feat> const& fs, CCNode* cl, CCMenu* menu, float W, float H, int row0) {
        m_feats = fs;
        m_valLabels.assign(fs.size(), nullptr);
        float colW = W / 2.f;
        int row = row0, col = 0;
        for (int i = 0; i < (int)fs.size(); i++) {
            auto const& f = fs[i];
            bool full = f.k != Feat::Toggle;
            if (full && col) { row++; col = 0; }
            float x0 = full ? 0.f : col * colW;
            float w = full ? W : colW;
            float y = H - 4.f - ROWH * (row + 0.5f);

            if (f.k == Feat::Button) {
                auto pill = makePill(f.label.c_str(), W * 0.6f, 22.f, 0.38f, C_BTN, 255);
                addIcon(menu, pill, W / 2, y, i, menu_selector(ModMenu::onButton));
            } else if (f.k == Feat::Note) {
                addText(cl, f.label, W / 2, y, W - 20.f, 0.38f, true)->setColor({150, 155, 185});
            } else {
                float lx = x0 + 8.f;
                bool hasToggle = f.k == Feat::Toggle || f.k == Feat::Input ||
                                 (f.k == Feat::Stepper && !f.key.empty());
                if (hasToggle) {
                    addCheck(menu, x0 + 16.f, y, optd(f.key.c_str(), f.def), i, menu_selector(ModMenu::onToggle));
                    lx = x0 + 34.f;
                }
                if (f.k == Feat::Toggle) {
                    float right = x0 + w - 4.f;
                    if (!f.gear.empty()) {
                        addIcon(menu, iconGear(), right - 11.f, y, i, menu_selector(ModMenu::onGear));
                        right -= 26.f;
                    }
                    addIcon(menu, iconInfo(), right - 8.f, y, i, menu_selector(ModMenu::onInfo));
                    addHold(cl, addText(cl, f.label, lx, y, right - 18.f - lx), f.label, descOf(f));
                } else if (f.k == Feat::Input) {
                    bool g = !f.gear.empty();
                    addHold(cl, addText(cl, f.label, lx, y, W * (g ? 0.46f : 0.6f) - lx), f.label, descOf(f));
                    if (g) addIcon(menu, iconGear(), W - 16.f, y, i, menu_selector(ModMenu::onGear));
                    float bw = W * (g ? 0.26f : 0.30f);
                    auto ti = TextInput::create(bw / 0.75f, "0", "bigFont.fnt");
                    ti->setScale(0.75f);
                    ti->setPosition({W * (g ? 0.72f : 0.80f), y});
                    ti->setCommonFilter(CommonFilter::Float);
                    ti->setString(fmt::format("{}", getVal(f.valKey.c_str(), f.vdef)));
                    std::string vk = f.valKey;
                    ti->setCallback([vk](std::string const& s) {
                        if (s.empty()) return;
                        setVal(vk.c_str(), std::strtod(s.c_str(), nullptr));
                    });
                    cl->addChild(ti, 3);
                } else if (f.k == Feat::Stepper) {
                    addHold(cl, addText(cl, f.label, lx, y, W - 112.f - lx), f.label, descOf(f));
                    auto mk = [&](char const* str, SEL_MenuHandler cb, float x) {
                        auto lab = CCLabelBMFont::create(str, "goldFont.fnt");
                        lab->setScale(0.8f);
                        addIcon(menu, lab, x, y, i, cb);
                    };
                    mk("-", menu_selector(ModMenu::onMinus), W - 106.f);
                    mk("+", menu_selector(ModMenu::onPlus), W - 18.f);
                    auto vl = addText(cl, fmtVal(f, getVal(f.valKey.c_str(), f.vdef)), W - 62.f, y, 62.f, 0.36f, true);
                    m_valLabels[i] = vl;
                }
            }
            if (full) row++;
            else if (++col == 2) { col = 0; row++; }
        }
        if (col) row++;
        return row;
    }

    // ── pages ──
    void refresh() {
        recolorTabs();
        m_content->removeAllChildren();
        m_valLabels.clear();
        m_feats.clear();

        if (!m_sub.empty()) buildSub();
        else if (m_tab == LABELS_TAB) buildLabels();
        else {
            auto const& fs = tabs()[m_tab].feats;
            CCMenu* menu; float H;
            auto cl = beginScroll(countRows(fs), menu, H);
            placeFeats(fs, cl, menu, m_cw, H, 0);
        }
    }

    void buildSub() {
        auto const& fs = subPage(m_sub);
        CCMenu* menu; float H;
        auto cl = beginScroll(countRows(fs) + 1, menu, H);
        float y = H - 4.f - ROWH * 0.5f;
        auto back = makePill("< Back", 70.f, 22.f, 0.38f, C_BTN, 255);
        addIcon(menu, back, 40.f, y, 0, menu_selector(ModMenu::onBack));
        addText(cl, m_subTitle, 86.f, y, m_cw - 96.f, 0.42f)->setColor(accentColor());
        placeFeats(fs, cl, menu, m_cw, H, 1);
    }

    void buildLabels() {
        std::vector<Feat> top = {
            Tg("Show Labels", "Master switch for all labels.", "", true),
            Tg("Show in Editor", "Show labels while in the editor."),
            Tg("Cheat Indicator", "A dot in the corner turns red once cheats were used.", "cheat"),
        };
        auto& order = lblOrder();
        int rows = 2 + 2 + 1 + (int)order.size();
        CCMenu* menu; float H;
        auto cl = beginScroll(rows, menu, H);
        float W = m_cw;
        placeFeats(top, cl, menu, W, H, 0);
        auto Y = [&](int r) { return H - 4.f - ROWH * (r + 0.5f); };

        addIcon(menu, makePill("Import Label", W * 0.88f, 23.f, 0.4f, C_BTN, 255), W / 2, Y(2), 0,
                menu_selector(ModMenu::onImport));
        addIcon(menu, makePill("Add New Label", W * 0.88f, 23.f, 0.4f, C_BTN, 255), W / 2, Y(3), 0,
                menu_selector(ModMenu::onAddLabel));

        addText(cl, "Presets", 8.f, Y(4), W * 0.4f, 0.42f);
        m_preset = std::clamp(m_preset, 0, (int)LBLS.size() - 1);
        auto dd = makePill(LBLS[m_preset].name, W * 0.40f, 22.f, 0.4f, C_INPUT, 255);
        {
            auto d = CCDrawNode::create();
            CCPoint t[3] = {{W * 0.40f - 18.f, 14.f}, {W * 0.40f - 8.f, 14.f}, {W * 0.40f - 13.f, 7.f}};
            d->drawPolygon(t, 3, {1.f, 1.f, 1.f, 1.f}, 0.f, {0, 0, 0, 0});
            dd->addChild(d);
        }
        addIcon(menu, dd, W * 0.77f, Y(4), 0, menu_selector(ModMenu::onPreset));

        for (int i = 0; i < (int)order.size(); i++) {
            float y = Y(5 + i);
            std::string id = order[i];
            addCheck(menu, 16.f, y, opt(("lbl_" + id).c_str()), i, menu_selector(ModMenu::onLblToggle));
            auto box = makeBox(W * 0.35f, 22.f, C_INPUT);
            box->setPosition({W * 0.09f + W * 0.175f, y});
            cl->addChild(box);
            addHold(cl, addText(cl, lblName(id), W * 0.265f, y, W * 0.32f, 0.36f, true), lblName(id), descByLabel(lblName(id)));
            addIcon(menu, iconArrow(false), W * 0.50f, y, i, menu_selector(ModMenu::onLblDown));
            addIcon(menu, iconArrow(true), W * 0.58f, y, i, menu_selector(ModMenu::onLblUp));
            addIcon(menu, iconGear(), W * 0.84f, y, i, menu_selector(ModMenu::onLblGear));
            addIcon(menu, iconTrash(), W * 0.94f, y, i, menu_selector(ModMenu::onLblDel));
        }
    }

    // ── callbacks ──
    void onTab(CCObject* s) {
        m_tab = static_cast<CCNode*>(s)->getTag();
        m_sub.clear();
        deferRefresh();
    }
    void onBack(CCObject*) { m_sub.clear(); deferRefresh(); }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        int i = t->getTag();
        if (i < 0 || i >= (int)m_feats.size()) return;
        auto const& f = m_feats[i];
        bool on = !t->isToggled(); // callback fires before the visual flips
        setOpt(f.key.c_str(), on);
        if (f.key == "autoldm") GameManager::get()->setGameVariable("0042", on);
        if (on && m_sub.empty() && !isImpl(f.key)) {
            Notification::create(f.label + ": not available yet", NotificationIcon::Warning, 1.5f)->show();
        } else if (opt("togglenotifications")) {
            Notification::create(fmt::format("{}: {}", f.label, on ? "ON" : "OFF"),
                                 NotificationIcon::None, 0.8f)->show();
        }
    }

    void onInfo(CCObject* s) {
        int i = static_cast<CCNode*>(s)->getTag();
        if (i < 0 || i >= (int)m_feats.size()) return;
        auto const& f = m_feats[i];
        Notification::create(f.label + ": " + descOf(f),
                             NotificationIcon::Info, 2.5f)->show();
    }

    void onGear(CCObject* s) {
        int i = static_cast<CCNode*>(s)->getTag();
        if (i < 0 || i >= (int)m_feats.size()) return;
        m_sub = m_feats[i].gear;
        m_subTitle = m_feats[i].label;
        deferRefresh();
    }

    void step(CCObject* s, int dir) {
        int i = static_cast<CCNode*>(s)->getTag();
        if (i < 0 || i >= (int)m_feats.size()) return;
        auto const& r = m_feats[i];
        double v = getVal(r.valKey.c_str(), r.vdef) + dir * r.step;
        v = std::clamp(v, r.vmin, r.vmax);
        v = std::round(v * 10.0) / 10.0;
        setVal(r.valKey.c_str(), v);
        if (i < (int)m_valLabels.size() && m_valLabels[i])
            m_valLabels[i]->setString(fmtVal(r, v).c_str());
        applyStyle();
    }
    void onMinus(CCObject* s) { step(s, -1); }
    void onPlus(CCObject* s) { step(s, +1); }

    void onButton(CCObject* s) {
        int i = static_cast<CCNode*>(s)->getTag();
        if (i < 0 || i >= (int)m_feats.size()) return;
        std::string k = m_feats[i].key;
        if (k == "resetdot") {
            if (m_dot) m_dot->resetPos(true);
        } else if (k.rfind("bot_", 0) == 0) {
            if (k == "bot_clear") {
                s_inputs.clear(); s_anchors.clear(); s_fixes = 0;
                Notification::create("Macro cleared", NotificationIcon::None, 1.f)->show();
            } else if (k == "bot_save") {
                bool ok = saveMacro();
                Notification::create(ok ? fmt::format("Saved {} inputs, {} anchors", s_inputs.size(), s_anchors.size())
                                        : std::string("Couldn't save macro"),
                                     ok ? NotificationIcon::Success : NotificationIcon::Error, 1.5f)->show();
            } else if (k == "bot_load") {
                bool ok = loadMacro();
                Notification::create(ok ? fmt::format("Loaded {} inputs, {} anchors", s_inputs.size(), s_anchors.size())
                                        : std::string("No saved macro found"),
                                     ok ? NotificationIcon::Success : NotificationIcon::Error, 1.5f)->show();
            } else if (k == "bot_anchor") {
                auto pl = PlayLayer::get();
                if (!pl) Notification::create("Only works inside a level", NotificationIcon::Warning, 1.2f)->show();
                else {
                    addAnchor(pl);
                    Notification::create(fmt::format("Anchor added at frame {}", botFrame(pl)),
                                         NotificationIcon::Success, 1.f)->show();
                }
            }
        } else if (k.rfind("sc_", 0) == 0) {
            auto pl = PlayLayer::get();
            if (k == "sc_hitbox") {
                setOpt("showhitboxes", !opt("showhitboxes"));
                Notification::create(fmt::format("Hitboxes: {}", opt("showhitboxes") ? "ON" : "OFF"),
                                     NotificationIcon::None, 0.8f)->show();
            } else if (k == "sc_noclip") {
                setOpt("noclip", !opt("noclip"));
                Notification::create(fmt::format("Noclip: {}", opt("noclip") ? "ON" : "OFF"),
                                     NotificationIcon::None, 0.8f)->show();
            } else if (!pl) {
                Notification::create("Only works inside a level", NotificationIcon::Warning, 1.2f)->show();
            } else if (k == "sc_restart") {
                pl->resetLevel();
            } else if (k == "sc_full") {
                pl->resetLevelFromStart();
            } else if (k == "sc_practice") {
                pl->togglePracticeMode(!pl->m_isPracticeMode);
            }
        } else if (k == "resetstyle") {
            setVal("accent", 0); setVal("menuop", 0.95); setVal("dotsize", 18); setVal("dotop", 0.6);
            applyStyle();
            deferRefresh();
        }
    }

    // labels page
    void onLblToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        auto& o = lblOrder();
        int i = t->getTag();
        if (i < 0 || i >= (int)o.size()) return;
        setOpt(("lbl_" + o[i]).c_str(), !t->isToggled());
    }
    void onLblUp(CCObject* s) {
        auto& o = lblOrder();
        int i = static_cast<CCNode*>(s)->getTag();
        if (i > 0 && i < (int)o.size()) { std::swap(o[i], o[i - 1]); saveOrder(); deferRefresh(); }
    }
    void onLblDown(CCObject* s) {
        auto& o = lblOrder();
        int i = static_cast<CCNode*>(s)->getTag();
        if (i >= 0 && i + 1 < (int)o.size()) { std::swap(o[i], o[i + 1]); saveOrder(); deferRefresh(); }
    }
    void onLblDel(CCObject* s) {
        auto& o = lblOrder();
        int i = static_cast<CCNode*>(s)->getTag();
        if (i >= 0 && i < (int)o.size()) { o.erase(o.begin() + i); saveOrder(); deferRefresh(); }
    }
    void onLblGear(CCObject* s) {
        auto& o = lblOrder();
        int i = static_cast<CCNode*>(s)->getTag();
        if (i < 0 || i >= (int)o.size()) return;
        m_sub = "labelstyle";
        m_subTitle = lblName(o[i]);
        deferRefresh();
    }
    void onPreset(CCObject*) {
        m_preset = (m_preset + 1) % (int)LBLS.size();
        deferRefresh();
    }
    void onAddLabel(CCObject*) {
        auto& o = lblOrder();
        std::string id = LBLS[std::clamp(m_preset, 0, (int)LBLS.size() - 1)].id;
        if (std::find(o.begin(), o.end(), id) != o.end()) {
            Notification::create("That label is already in the list", NotificationIcon::Warning, 1.2f)->show();
            return;
        }
        o.push_back(id);
        saveOrder();
        deferRefresh();
    }
    void onImport(CCObject*) {
        Notification::create("Label import isn't supported yet", NotificationIcon::Warning, 1.5f)->show();
    }

    void update(float dt) override {
        auto sched = CCDirector::get()->getScheduler();
        bool inLevel = PlayLayer::get() != nullptr;

        float want = 1.f;
        if (inLevel && opt("speedhack") && (!opt("speed_practice") || PlayLayer::get()->m_isPracticeMode))
            want = (float)std::max(0.01, getVal("speed", 1.0));
        if (sched->getTimeScale() != want) sched->setTimeScale(want);

        float scale = sched->getTimeScale();
        if (dt > 0.f) m_fps = m_fps * 0.9f + (scale / dt) * 0.1f;
        s_fps = m_fps;

        applyAudio(inLevel);

        s_hue += dt * 0.2f;
        if ((int)getVal("accent", 0) >= 10) applyLight(); // rainbow
    }

    void hidePanel() { if (!m_open) m_panel->setVisible(false); }

    void toggle() {
        m_open = !m_open;
        m_panel->stopAllActions();
        auto td = CCTouchDispatcher::get();
        if (m_open) {
            m_panel->setVisible(true);
            td->removeDelegate(this);
            td->addTargetedDelegate(this, -90, true);
            auto sc = CCScaleTo::create(0.25f, 1.f);
            m_panel->runAction(CCEaseBackOut::create(sc));
        } else {
            td->removeDelegate(this);
            auto sc = CCScaleTo::create(0.18f, 0.01f);
            m_panel->runAction(CCSequence::create(
                CCEaseIn::create(sc, 2.f),
                CCCallFunc::create(this, callfunc_selector(ModMenu::hidePanel)),
                nullptr));
        }
    }
};

inline bool HoldLabel::ccTouchBegan(CCTouch* t, CCEvent*) {
    if (!optd("holdinfo", true)) return false;
    auto mm = ModMenu::s_inst;
    if (!mm || !mm->m_open || !mm->inScroll(t)) return false;
    if (!m_target || !m_target->getParent() || !m_target->isVisible()) return false;
    auto p = m_target->getParent()->convertTouchToNodeSpace(t);
    auto r = m_target->boundingBox();
    r.origin.x -= 4.f; r.origin.y -= 4.f; r.size.width += 8.f; r.size.height += 8.f;
    if (!r.containsPoint(p)) return false;
    m_down = true; m_fired = false; m_t = 0.f;
    m_start = t->getLocation();
    return true;
}

// ───────────────────────── pause menu confirmations ─────────────────────────
class $modify(HMPauseLayer, PauseLayer) {
    void onRestart(CCObject* s) {
        if (!opt("confirmrestart") || s_confirmBypass) return PauseLayer::onRestart(s);
        createQuickPopup("Restart", "Restart the level?", "No", "Yes", [this, s](FLAlertLayer*, bool yes) {
            if (!yes) return;
            s_confirmBypass = true;
            PauseLayer::onRestart(s);
            s_confirmBypass = false;
        });
    }
    void onRestartFull(CCObject* s) {
        if (!opt("confirmfullrestart") || s_confirmBypass) return PauseLayer::onRestartFull(s);
        createQuickPopup("Restart From Start", "Restart from the beginning?", "No", "Yes", [this, s](FLAlertLayer*, bool yes) {
            if (!yes) return;
            s_confirmBypass = true;
            PauseLayer::onRestartFull(s);
            s_confirmBypass = false;
        });
    }
    void onQuit(CCObject* s) {
        if (!opt("confirmquit") || s_confirmBypass) return PauseLayer::onQuit(s);
        createQuickPopup("Quit", "Quit the level?", "No", "Yes", [this, s](FLAlertLayer*, bool yes) {
            if (!yes) return;
            s_confirmBypass = true;
            PauseLayer::onQuit(s);
            s_confirmBypass = false;
        });
    }
    void onPracticeMode(CCObject* s) {
        if (!opt("confirmpracticemode") || s_confirmBypass) return PauseLayer::onPracticeMode(s);
        createQuickPopup("Practice Mode", "Enter practice mode?", "No", "Yes", [this, s](FLAlertLayer*, bool yes) {
            if (!yes) return;
            s_confirmBypass = true;
            PauseLayer::onPracticeMode(s);
            s_confirmBypass = false;
        });
    }
};

class $modify(MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (!ModMenu::s_inst) {
            ModMenu::s_inst = ModMenu::create();
            if (ModMenu::s_inst) {
                ModMenu::s_inst->setZOrder(100000);
                OverlayManager::get()->addChild(ModMenu::s_inst);
                Notification::create("Hitbox Menu loaded", NotificationIcon::Success, 2.f)->show();
            }
        }
        return true;
    }
};

class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        if (down && !repeat && key == KEY_Tab && ModMenu::s_inst) {
            ModMenu::s_inst->toggle();
            return true;
        }
        // keyboard shortcuts (Windows); keys are set in the Shortcuts tab
        if (down && !repeat) {
            auto pl = PlayLayer::get();
            if (keyIs(key, "key_noclip", 1)) {
                setOpt("noclip", !opt("noclip"));
                Notification::create(fmt::format("Noclip: {}", opt("noclip") ? "ON" : "OFF"), NotificationIcon::None, 0.8f)->show();
                return true;
            }
            if (keyIs(key, "key_hitbox", 2)) {
                setOpt("showhitboxes", !opt("showhitboxes"));
                Notification::create(fmt::format("Hitboxes: {}", opt("showhitboxes") ? "ON" : "OFF"), NotificationIcon::None, 0.8f)->show();
                return true;
            }
            if (pl && keyIs(key, "key_restart", 3)) { pl->resetLevel(); return true; }
            if (pl && keyIs(key, "key_practice", 4)) { pl->togglePracticeMode(!pl->m_isPracticeMode); return true; }
            if (pl && keyIs(key, "key_full", 0)) { pl->resetLevelFromStart(); return true; }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

// ───────────────────────── small hooks ─────────────────────────
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this && !s_injecting) {
            int mode = (int)getVal("botmode", 0);
            if (mode == 2) return; // replaying: real taps are ignored
            if (mode == 1) s_inputs.push_back({botFrame(this), down, button, isPlayer1});
        }
        if (down) { s_clicks.push_back(nowSec()); s_attemptClicks++; }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
    // Physics Bypass: while stepping, every step is 1/TPS seconds long
    float getModifiedDelta(float dt) {
        float r = GJBaseGameLayer::getModifiedDelta(dt);
        if (s_tpsInner) return (float)(1.0 / tpsVal()) * std::min(m_gameState.m_timeWarp, 1.f);
        return r;
    }
    // Physics Bypass: runs the game update once per physics tick at the chosen TPS
    void update(float dt) {
        if (s_tpsInner || !tpsActive(this)) return GJBaseGameLayer::update(dt);
        double T = tpsVal();
        s_tpsAcc += (double)dt * T;
        int n = std::min((int)s_tpsAcc, 1000);
        s_tpsAcc -= (double)n;
        if (s_tpsAcc > 1.0) s_tpsAcc = 0.0;
        s_tpsInner = true;
        for (int i = 0; i < n; i++) GJBaseGameLayer::update((float)(1.0 / T));
        s_tpsInner = false;
    }
    void processCommands(float dt) {
        botStep(this);
        GJBaseGameLayer::processCommands(dt);
    }
};

static bool ncBase(PlayLayer* pl, PlayerObject* p) {
    bool on = opt("noclip") || (opt("noclip_p1") && p == pl->m_player1) ||
              (opt("noclip_p2") && p == pl->m_player2);
    if (!on) return false;
    if (opt("nc_practice") && !pl->m_isPracticeMode) return false;
    float pct = pl->getCurrentPercent();
    if (opt("nc_after") && pct < getVal("ncafter", 0.0)) return false;
    if (opt("nc_until") && pct > getVal("ncuntil", 100.0)) return false;
    return true;
}

class $modify(PlayerObject) {
    void playDeathEffect() {
        if (opt("nodeatheffect")) {
            auto pl = PlayLayer::get();
            if (!opt("nde_practice") || (pl && pl->m_isPracticeMode)) return;
        }
        PlayerObject::playDeathEffect();
    }
    // Fallback: some solid-block deaths don't go through the PlayLayer::destroyPlayer hook,
    // so block the death itself here when noclip applies.
    void playerDestroyed(bool b) {
        auto pl = PlayLayer::get();
        if (pl && !s_forceKill && !opt("nc_hazonly") && ncBase(pl, this)) {
            s_extBlock = true;
            return;
        }
        PlayerObject::playerDestroyed(b);
    }
};

class $modify(GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (opt("unlockicons")) return true;
        return GameManager::isIconUnlocked(id, type);
    }
    bool isColorUnlocked(int id, UnlockType type) {
        if (opt("unlockicons") && optd("ui_colors", true)) return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

// ───────────────────────── gameplay / labels ─────────────────────────
class $modify(HMPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* trailNode = nullptr;
        CCDrawNode* hitNode = nullptr;
        CCDrawNode* cheatDot = nullptr;
        CCDrawNode* anchorNode = nullptr;
        std::deque<std::vector<CCRect>> hist;
        std::map<std::string, CCLabelBMFont*> labels;
        CCLayerColor* flash = nullptr;
        bool cheated = false;
        bool blockedThisFrame = false;
        bool prevBlocked = false;
        bool frozenHb = false;
        int totalFrames = 0;
        int blockedFrames = 0;
        int ncDeaths = 0;
        int attempts = 1;
        float levelTime = 0.f;
        float best = 0.f;
        float runFrom = 0.f;
        float clickTimer = 0.f;
        bool clickHeld = false;
        bool instDone = false;
    };

    bool isSafe() {
        return opt("safemode") || (optd("autosafemode", true) && m_fields->cheated);
    }

    bool ncActive(PlayerObject* p, GameObject* obj) {
        if (!ncBase(this, p)) return false;
        if (obj) {
            bool haz = obj->m_objectType == GameObjectType::Hazard;
            if (opt("nc_hazonly") && !haz) return false;
            if (opt("nc_solidonly") && haz) return false;
        }
        return true;
    }

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (obj != m_anticheatSpike && ncActive(p, obj)) {
            m_fields->blockedThisFrame = true;
            return;
        }
        bool wasDead = m_player1 && m_player1->m_isDead;
        PlayLayer::destroyPlayer(p, obj);
        bool justDied = !wasDead && m_player1 && m_player1->m_isDead;

        // Death Percent Popup: shows where you died
        if (justDied && opt("deathpercentpopup"))
            Notification::create(fmt::format("Died at {:.0f}%", this->getCurrentPercent()), NotificationIcon::None, 1.f)->show();

        // Mute Level Audio on Death: pauses the music until you respawn
        if (justDied && opt("mutelevelaudioondeath")) {
            auto fm = FMODAudioEngine::sharedEngine();
            if (fm && fm->m_backgroundMusicChannel) { fm->m_backgroundMusicChannel->setPaused(true); s_audioMuted = true; }
        }

        // Respawn Delay: cancel the game's own 1s respawn timer and use ours (can be shorter or longer)
        if (opt("respawndelay") && justDied) {
            this->stopActionByTag(0x10);
            float d = (float)std::max(0.0, getVal("respawndelay_v", 1.0));
            this->runAction(CCSequence::create(
                CCDelayTime::create(d),
                CCCallFunc::create(this, callfunc_selector(PlayLayer::delayedResetLevel)),
                nullptr));
        }

        if (opt("hitboxesondeath") && !opt("showhitboxes")) {
            auto f = m_fields.self();
            ensureNodes();
            f->hitNode->clear();
            drawHitboxes(false);
            f->frozenHb = true; // keep the snapshot on screen until the next attempt
        }
    }

    void ensureNodes() {
        auto f = m_fields.self();
        if (!f->trailNode) {
            f->trailNode = CCDrawNode::create();
            m_objectLayer->addChild(f->trailNode, 1000);
            f->hitNode = CCDrawNode::create();
            m_objectLayer->addChild(f->hitNode, 1001);
            f->anchorNode = CCDrawNode::create();
            m_objectLayer->addChild(f->anchorNode, 1002);
        }
    }

    void drawHitboxes(bool blocked) {
        auto f = m_fields.self();
        if (!m_player1) return;
        float thick = (float)getVal("hbthick", 1.0);
        bool fill = opt("fill");
        auto hz = f4(colIdx("hazcol", 2), 1.f);
        auto so = f4(colIdx("solidcol", 7), 1.f);
        auto pl = (opt("nc_redhit") && blocked) ? ccColor4F{1.f, 0.f, 0.f, 1.f}
                                                : f4(colIdx("plrcol", 4), 1.f);
        float px = m_player1->getPositionX();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            float dx = obj->getPositionX() - px;
            if (dx < -250.f || dx > 450.f) continue;
            ccColor4F col;
            if (obj->m_objectType == GameObjectType::Hazard) col = hz;
            else if (obj->m_objectType == GameObjectType::Solid) col = so;
            else continue;
            drawRect(f->hitNode, obj->getObjectRect(), col, fill, thick);
        }
        drawRect(f->hitNode, m_player1->getObjectRect(), pl, fill, thick);
        if (m_gameState.m_isDualMode && m_player2)
            drawRect(f->hitNode, m_player2->getObjectRect(), pl, fill, thick);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (opt("autopracticemode") && !m_isPracticeMode) this->togglePracticeMode(true);
    }

    void resetLevel() {
        auto f = m_fields.self();
        f->hist.clear();
        f->frozenHb = false;
        if (f->hitNode) f->hitNode->clear();
        s_attemptClicks = 0;
        if (!opt("freezeattempts")) f->attempts++;
        f->levelTime = 0.f;
        f->totalFrames = 0;
        f->blockedFrames = 0;
        f->ncDeaths = 0;
        f->prevBlocked = false;
        f->instDone = false;
        f->clickHeld = false;
        f->clickTimer = 0.f;
        PlayLayer::resetLevel();
        if (s_audioMuted) {
            s_audioMuted = false;
            auto fm = FMODAudioEngine::sharedEngine();
            if (fm && fm->m_backgroundMusicChannel) fm->m_backgroundMusicChannel->setPaused(false);
        }
        f->runFrom = this->getCurrentPercent();
        int bm = (int)getVal("botmode", 0);
        if (bm == 1) {
            int cur = m_isPracticeMode ? (int)m_gameState.m_currentProgress : 0;
            s_inputs.erase(std::remove_if(s_inputs.begin(), s_inputs.end(),
                           [cur](BInput const& b) { return b.frame >= cur; }), s_inputs.end());
            s_anchors.erase(std::remove_if(s_anchors.begin(), s_anchors.end(),
                            [cur](BotAnchor const& a) { return a.frame >= cur; }), s_anchors.end());
        } else if (bm == 2 && !m_isPracticeMode) s_fixes = 0;
    }

    // Hide Level Complete VFX: skips the "Level Complete!" text and its effects
    void showCompleteText() {
        if (opt("hidelevelcompletevfx")) return;
        PlayLayer::showCompleteText();
    }

    // Layout Mode / No Object Glow: re-applied every frame because the game re-shows objects while culling
    void applyObjectLook() {
        bool lay = opt("layoutmode"), glow = opt("noobjectglow");
        if (!lay && !glow) return;
        if (!m_player1) return;
        float px = m_player1->getPositionX();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (!obj) continue;
            float dx = obj->getPositionX() - px;
            if (dx < -300.f || dx > 900.f) continue;
            if (lay && obj->m_objectType == GameObjectType::Decoration) obj->setVisible(false);
            if (glow && obj->m_glowSprite) obj->m_glowSprite->setVisible(false);
        }
    }

    // Safe mode: completions/progress are not saved once cheats were used
    void levelComplete() {
        if (isSafe()) {
            m_isTestMode = true;
            if (opt("sm_notify"))
                Notification::create("Safe Mode: this completion won't be saved", NotificationIcon::Warning, 2.f)->show();
        }
        PlayLayer::levelComplete();
    }
    void onQuit() {
        if (isSafe()) m_isTestMode = true;
        s_audioMuted = false;
        PlayLayer::onQuit();
    }

    void makeLabels() {
        auto f = m_fields.self();
        auto w = CCDirector::get()->getWinSize();
        f->flash = CCLayerColor::create({255, 0, 0, 0}, w.width, w.height);
        m_uiLayer->addChild(f->flash, 40);
        for (auto const& d : LBLS) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setAnchorPoint({0.f, 1.f});
            l->setVisible(false);
            m_uiLayer->addChild(l, 50);
            f->labels[d.id] = l;
        }
        f->cheatDot = CCDrawNode::create();
        f->cheatDot->setPosition({w.width - 14.f, w.height - 14.f});
        m_uiLayer->addChild(f->cheatDot, 50);
    }

    float accuracy() {
        auto f = m_fields.self();
        return f->totalFrames > 0 ? 100.f * (1.f - float(f->blockedFrames) / float(f->totalFrames)) : 100.f;
    }

    void updateLabels(float pct, bool blocked) {
        auto f = m_fields.self();
        if (f->labels.empty()) makeLabels();
        auto win = CCDirector::get()->getWinSize();

        double now = nowSec();
        while (!s_clicks.empty() && now - s_clicks.front() > 1.0) s_clicks.pop_front();

        // noclip flash
        if (f->flash) {
            int o = f->flash->getOpacity();
            if (opt("ncflash") && blocked) {
                f->flash->setColor(colIdx("flashcol", 2));
                o = (int)(255.0 * getVal("flashop", 0.4));
            } else o = std::max(0, o - 15);
            f->flash->setOpacity((GLubyte)o);
        }

        bool master = optd("showlabels", true);

        // cheat indicator
        if (f->cheatDot) {
            f->cheatDot->clear();
            int cc = std::clamp((int)getVal("chcorner", 1.0), 0, 3);
            f->cheatDot->setPosition(CCPoint((cc % 2 == 0) ? 14.f : win.width - 14.f, (cc < 2) ? win.height - 14.f : 14.f));
            if (master && opt("cheatindicator")) {
                ccColor4F c = f->cheated ? ccColor4F{1.f, 0.2f, 0.2f, 0.9f} : ccColor4F{0.3f, 1.f, 0.4f, 0.9f};
                f->cheatDot->drawDot({0, 0}, (float)getVal("chdot", 5.0), c);
            }
        }

        float sc = 0.32f * (float)getVal("lblscale", 1.0);
        auto lc = colIdx("lblcol", 8);
        GLubyte lo = (GLubyte)(255.0 * getVal("lblop", 0.8));
        float y = win.height - 6.f;

        for (auto& kv : f->labels) kv.second->setVisible(false);

        for (auto const& id : lblOrder()) {
            auto it = f->labels.find(id);
            if (it == f->labels.end()) continue;
            auto lab = it->second;
            bool show = master && opt(("lbl_" + id).c_str());
            std::string txt;
            if (id == "fps") txt = fmt::format("FPS: {}", (int)(s_fps + 0.5f));
            else if (id == "pct")
                txt = opt("accuratepercentage") ? fmt::format("{:.{}f}%", pct, std::clamp((int)getVal("accdec", 2.0), 1, 3)) : fmt::format("{}%", (int)pct);
            else if (id == "attempt") txt = fmt::format("Attempt {}", f->attempts);
            else if (id == "time") {
                int m = (int)(f->levelTime / 60.f);
                txt = fmt::format("{}:{:05.2f}", m, f->levelTime - m * 60.f);
            }
            else if (id == "best") txt = fmt::format("Best: {:.1f}%", f->best);
            else if (id == "runfrom") txt = fmt::format("Run From: {:.0f}%", f->runFrom);
            else if (id == "clock") {
                std::time_t t = std::time(nullptr);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
                txt = buf;
            }
            else if (id == "cps") txt = fmt::format("CPS: {}", s_clicks.size());
            else if (id == "nca") txt = fmt::format("Noclip Accuracy: {:.2f}%", accuracy());
            else if (id == "ncd") txt = fmt::format("Noclip Deaths: {}", f->ncDeaths);
            else if (id == "bot") {
                int bm = (int)getVal("botmode", 0);
                char const* names[3] = {"Off", "Recording", "Replaying"};
                txt = fmt::format("Bot {}: {} inputs, {} anchors, {} fixes", names[std::clamp(bm, 0, 2)],
                                  s_inputs.size(), s_anchors.size(), s_fixes);
            }
            else if (id == "frame") txt = fmt::format("Frame: {}", (int)m_gameState.m_currentProgress);
            else if (id == "pos")
                txt = m_player1 ? fmt::format("X: {:.1f}  Y: {:.1f}", m_player1->getPositionX(), m_player1->getPositionY()) : std::string();
            else if (id == "yvel")
                txt = m_player1 ? fmt::format("Y Vel: {:.2f}", m_player1->m_yVelocity) : std::string();
            else if (id == "speed")
                txt = fmt::format("Speed: {:.2f}x", opt("speedhack") ? getVal("speed", 1.0) : 1.0);
            else if (id == "tps") txt = fmt::format("TPS: {}", tpsActive(this) ? (int)tpsVal() : 240);
            else if (id == "jumps") txt = fmt::format("Jumps: {}", s_attemptClicks);
            else if (id == "level") txt = m_level ? std::string(m_level->m_levelName) : std::string("Level");
            else if (id == "testmode") {
                if (isSafe()) txt = "Safe Mode";
                else if (m_isTestMode) txt = "Testmode";
                else show = false;
            }
            lab->setVisible(show);
            if (!show) continue;
            lab->setScale(sc);
            lab->setColor(lc);
            lab->setOpacity(lo);
            lab->setString(txt.c_str());
            lab->setPosition({62.f, y});
            y -= 35.f * sc;
        }
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();

        ensureNodes();

        // read the flag BEFORE resetting it (used by flash / hitbox colour)
        bool blocked = f->blockedThisFrame || s_extBlock;
        s_extBlock = false;

        {
            bool nc = opt("noclip") || opt("noclip_p1") || opt("noclip_p2");
            bool sp = opt("speedhack") && std::fabs(getVal("speed", 1.0) - 1.0) > 0.001;
            if ((nc && optd("asm_noclip", true)) || (opt("jumphack") && optd("asm_jump", true)) ||
                (sp && optd("asm_speed", true)) || opt("autoclicker") || opt("instantcomplete") ||
            (opt("physicsbypass") && std::fabs(getVal("physicsbypass_v", 240.0) - 240.0) > 0.5) || ((int)getVal("botmode", 0) == 2 && optd("asm_bot", true)))
                f->cheated = true;
        }

        f->totalFrames++;
        if (blocked) {
            f->blockedFrames++;
            if (!f->prevBlocked) f->ncDeaths++;
        }
        f->prevBlocked = blocked;
        f->blockedThisFrame = false;

        // noclip limits: kill the player when accuracy / death count is exceeded
        if (m_player1 && !m_player1->m_isDead) {
            if ((opt("nclimit") && f->totalFrames > 60 && accuracy() < getVal("nclimitv", 95.0)) ||
                (opt("ncdlimit") && f->ncDeaths >= (int)getVal("ncdlimitv", 10.0)))
            {
                s_forceKill = true;
                PlayLayer::destroyPlayer(m_player1, m_anticheatSpike);
                s_forceKill = false;
            }
        }

        if (m_player1 && !m_player1->m_isDead) f->levelTime += dt;
        float pct = this->getCurrentPercent();
        if (pct > f->best) f->best = pct;

        if (opt("jumphack") && (!opt("jh_practice") || m_isPracticeMode)) {
            if (m_player1) m_player1->m_isOnGround = true;
            if (m_player2 && !opt("jh_p1only")) m_player2->m_isOnGround = true;
        }

        // Auto Kill: kills you once the level reaches the chosen percentage
        if (opt("autokill") && m_player1 && !m_player1->m_isDead) {
            double at = getVal("autokillpct", 100.0);
            if (at < 100.0 && pct >= at) {
                s_forceKill = true;
                PlayLayer::destroyPlayer(m_player1, m_anticheatSpike);
                s_forceKill = false;
            }
        }

        // AutoClicker: taps (and releases) player 1's button at the chosen clicks per second
        if (opt("autoclicker") && m_player1 && !m_player1->m_isDead) {
            f->clickTimer += dt;
            double iv = 1.0 / std::max(0.5, getVal("autoclickcps", 10.0));
            if (f->clickHeld) {
                this->handleButton(false, 1, true);
                f->clickHeld = false;
            } else if (f->clickTimer >= iv) {
                f->clickTimer = 0.f;
                this->handleButton(true, 1, true);
                f->clickHeld = true;
            }
        }

        // Instant Complete: finishes the level a few frames after it starts
        if (opt("instantcomplete") && !f->instDone && f->totalFrames > 10 && m_player1 && !m_player1->m_isDead) {
            f->instDone = true;
            this->levelComplete();
        }

        applyObjectLook();

        // player effect removers
        auto fx = [&](PlayerObject* p) {
            if (!p) return;
            if (opt("norobotfire") && p->m_robotFire) p->m_robotFire->setVisible(false);
            if (opt("noghosttrail") && p->m_ghostTrail) p->m_ghostTrail->setVisible(false);
            if (opt("nowavetrailbehind") && p->m_waveTrail) p->m_waveTrail->setVisible(false);
            if (opt("solidwavetrail") && p->m_waveTrail) p->m_waveTrail->m_isSolid = true;
            if (opt("novehicleparticles") || opt("noparticles")) {
                if (p->m_vehicleGroundParticles) p->m_vehicleGroundParticles->setVisible(false);
                if (p->m_shipClickParticles) p->m_shipClickParticles->setVisible(false);
                if (p->m_playerGroundParticles) p->m_playerGroundParticles->setVisible(false);
            }
        };
        fx(m_player1);
        fx(m_player2);

        bool hide = opt("hideplayer");
        if (m_player1) m_player1->setVisible(!(hide && !opt("hp_keepp1")));
        if (m_player2) m_player2->setVisible(!(hide && !opt("hp_keepp2")));

        updateLabels(pct, blocked);

        float thick = (float)getVal("hbthick", 1.0);

        // no trail
        if (opt("notrail")) {
            if (m_player1 && m_player1->m_regularTrail) m_player1->m_regularTrail->setVisible(false);
            if (m_player2 && optd("nt_p2", true) && m_player2->m_regularTrail) m_player2->m_regularTrail->setVisible(false);
        }

        // ── hitboxes (drawn manually) ──
        if (!f->frozenHb) {
            f->hitNode->clear();
            if (opt("showhitboxes") || (opt("hitboxesondeath") && m_player1 && m_player1->m_isDead))
                drawHitboxes(blocked);
        }

        // ── anchors ──
        if (f->anchorNode) {
            f->anchorNode->clear();
            if (opt("showanchors") && m_player1) {
                float px = m_player1->getPositionX();
                for (auto const& a : s_anchors) {
                    if (std::fabs(a.p1.x - px) > 500.f) continue;
                    f->anchorNode->drawDot(CCPoint(a.p1.x, a.p1.y), 3.f, ccColor4F{1.f, 0.8f, 0.2f, 0.9f});
                }
            }
        }

        // ── trail ──
        f->trailNode->clear();
        if (!opt("trail")) { f->hist.clear(); return; }

        if (m_player1 && !m_player1->m_isDead) {
            std::vector<CCRect> r { m_player1->getObjectRect() };
            if (m_gameState.m_isDualMode && m_player2) r.push_back(m_player2->getObjectRect());
            f->hist.push_back(std::move(r));
            size_t maxLen = opt("customtrail") ? (size_t)getVal("traillen", 240.0) : TRAIL_LEN;
            while (f->hist.size() > maxLen) f->hist.pop_front();
        }

        auto tc = colIdx("trailcol", 2);
        size_t n = f->hist.size();
        for (size_t i = 0; i < n; i += TRAIL_STEP) {
            float a = 0.15f + 0.8f * (float(i + 1) / float(n));
            for (size_t j = 0; j < f->hist[i].size(); j++) {
                ccColor4F col = j == 0 ? f4(tc, a) : ccColor4F{0.2f, 0.5f, 1.f, a};
                drawRect(f->trailNode, f->hist[i][j], col, false, thick);
            }
        }
    }
};
7
