#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

// ───────────────────────── settings helpers ─────────────────────────
static bool optd(char const* k, bool d) { return Mod::get()->getSavedValue<bool>(k, d); }
static bool opt(char const* k) { return optd(k, false); }
static void setOpt(char const* k, bool v) { Mod::get()->setSavedValue<bool>(k, v); }
static double getVal(char const* k, double d) { return Mod::get()->getSavedValue<double>(k, d); }
static void setVal(char const* k, double v) { Mod::get()->setSavedValue<double>(k, v); }

static constexpr size_t TRAIL_LEN = 240;
static constexpr size_t TRAIL_STEP = 3;

static float s_fps = 60.f;
static float s_hue = 0.f;
static std::deque<double> s_clicks;

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
    "Cyan", "Green", "Red", "Orange", "Yellow", "Purple", "Pink", "Blue", "White", "Rainbow"
};
static ccColor3B const PANELS[6] = {
    {0, 0, 0}, {10, 15, 50}, {35, 10, 55}, {55, 10, 15}, {10, 40, 20}, {45, 45, 50}
};
static std::vector<char const*> const PANELNAMES = {"Black", "Navy", "Purple", "Maroon", "Forest", "Gray"};
static std::vector<char const*> const THEMENAMES = {"Dark", "Classic", "Glass", "Sharp", "Neon", "Gradient"};

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
    return i >= 9 ? hsv(s_hue) : COLS[std::clamp(i, 0, 8)];
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

// ───────────────────────── menu definition ─────────────────────────
struct Row {
    char const* key = "";
    char const* label = "";
    bool def = false;              // default toggle state
    char const* valKey = nullptr;  // stepper value key
    float vmin = 0, vmax = 0, step = 0, vdef = 0;
    std::vector<char const*> names; // stepper shows names instead of numbers
    bool plain = false;            // stepper only, no toggle
    bool button = false;           // push button
};
struct Tab { char const* name; std::vector<Row> rows; };

static Row T(char const* key, char const* label, bool def = false) {
    Row r; r.key = key; r.label = label; r.def = def; return r;
}
static Row S(char const* key, char const* label, char const* vk, float mn, float mx, float st, float vd) {
    Row r; r.key = key; r.label = label; r.valKey = vk; r.vmin = mn; r.vmax = mx; r.step = st; r.vdef = vd; return r;
}
static Row P(char const* label, char const* vk, float mn, float mx, float st, float vd,
             std::vector<char const*> names = {}) {
    Row r; r.key = vk; r.label = label; r.valKey = vk; r.vmin = mn; r.vmax = mx; r.step = st; r.vdef = vd;
    r.names = std::move(names); r.plain = true; return r;
}
static Row B(char const* key, char const* label) {
    Row r; r.key = key; r.label = label; r.button = true; return r;
}

static std::vector<Tab> const& tabs() {
    static std::vector<Tab> t = {
        {"Global", {
            S("speedhack", "Speedhack", "speed", 0.1f, 5.f, 0.1f, 1.f),
            T("safemode", "Safe Mode"),
            T("autosafe", "Auto Safe Mode", true),
            T("unlockicons", "Unlock Icons"),
            T("notify", "Toggle Notifications"),
            B("resetdot", "Reset Dot Position"),
        }},
        {"Level", {
            T("hitbox", "Show Hitboxes"),
            T("hitdeath", "Hitboxes On Death"),
            T("fill", "Fill Hitboxes"),
            T("trail", "Hitbox Trail"),
            S("customtrail", "Trail Length", "traillen", 30.f, 600.f, 30.f, 240.f),
            T("autopractice", "Auto Practice Mode"),
            T("autoldm", "Auto LDM"),
            P("Hazard Color", "hazcol", 0, 8, 1, 2, COLNAMES),
            P("Solid Color", "solidcol", 0, 8, 1, 7, COLNAMES),
            P("Player Color", "plrcol", 0, 8, 1, 4, COLNAMES),
            P("Trail Color", "trailcol", 0, 8, 1, 2, COLNAMES),
            P("Line Thickness", "hbthick", 0.5f, 3.f, 0.5f, 1.f),
        }},
        {"Player", {
            T("noclip", "Noclip"),
            T("noclip_p1", "Noclip P1 Only"),
            T("noclip_p2", "Noclip P2 Only"),
            T("jumphack", "Jump Hack"),
            T("nodeatheffect", "No Death Effect"),
            T("hideplayer", "Hide Player"),
        }},
        {"Noclip", {
            T("ncflash", "Noclip Flash"),
            P("Flash Color", "flashcol", 0, 8, 1, 2, COLNAMES),
            P("Flash Opacity", "flashop", 0.1f, 1.f, 0.1f, 0.4f),
            T("nc_practice", "Only In Practice"),
            T("nc_hazonly", "Pass Hazards Only"),
            T("nc_solidonly", "Pass Solids Only"),
            S("nc_after", "Start At %", "ncafter", 0.f, 100.f, 5.f, 0.f),
            S("nc_until", "Stop At %", "ncuntil", 0.f, 100.f, 5.f, 100.f),
            S("nclimit", "Min Accuracy %", "nclimitv", 50.f, 100.f, 1.f, 95.f),
            S("ncdlimit", "Max NC Deaths", "ncdlimitv", 1.f, 100.f, 1.f, 10.f),
            T("nc_redhit", "Red Hitbox On Block"),
        }},
        {"Labels", {
            T("lbl_master", "Show Labels", true),
            S("lbl_size", "Label Size", "lblscale", 0.5f, 2.f, 0.1f, 1.f),
            P("Label Color", "lblcol", 0, 8, 1, 8, COLNAMES),
            P("Label Opacity", "lblop", 0.1f, 1.f, 0.1f, 0.8f),
            T("lbl_fps", "FPS"),
            T("lbl_pct", "Percentage"),
            T("lbl_acc", "Accurate Percentage"),
            T("lbl_attempt", "Attempt"),
            T("lbl_time", "Level Time"),
            T("lbl_best", "Best Run"),
            T("lbl_clock", "Clock"),
            T("lbl_cps", "CPS"),
            T("lbl_nca", "Noclip Accuracy"),
            T("lbl_ncd", "Noclip Deaths"),
            T("lbl_test", "Testmode / Safe Mode"),
        }},
        {"Style", {
            P("Theme", "theme", 0, 5, 1, 0, THEMENAMES),
            P("Accent", "accent", 0, 9, 1, 0, ACCNAMES),
            P("Panel Color", "panelcol", 0, 5, 1, 0, PANELNAMES),
            P("Menu Opacity", "menuop", 0.2f, 1.f, 0.1f, 0.8f),
            P("Dot Size", "dotsize", 10, 30, 2, 18),
            P("Dot Opacity", "dotop", 0.1f, 1.f, 0.1f, 0.6f),
            B("resetstyle", "Reset Style"),
        }},
    };
    return t;
}

static std::string fmtVal(Row const& r, double v) {
    if (!r.names.empty()) {
        int i = std::clamp((int)std::lround(v), 0, (int)r.names.size() - 1);
        return r.names[i];
    }
    return r.step >= 1.f ? fmt::format("{:.0f}", v) : fmt::format("{:.1f}", v);
}

static CCNode* makePill(char const* text, float w, float h, float fs, ccColor3B col, GLubyte op) {
    auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
    bg->setContentSize({w, h});
    bg->setColor(col);
    bg->setOpacity(op);
    auto l = CCLabelBMFont::create(text, "bigFont.fnt");
    l->setScale(fs);
    l->setPosition({w / 2, h / 2});
    l->setTag(77);
    bg->addChild(l);
    return bg;
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
class ModMenu : public CCNode {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    CCNode* m_bg = nullptr;
    CCDrawNode* m_border = nullptr;
    CCLabelBMFont* m_title = nullptr;
    CCNode* m_content = nullptr;
    DotNode* m_dot = nullptr;
    std::vector<CCNode*> m_tabPills;
    std::vector<CCLabelBMFont*> m_valLabels;
    int m_tab = 0;
    bool m_open = false;
    float m_fps = 60.f;
    CCSize m_ps {540.f, 262.f};

    static ModMenu* create() {
        auto r = new ModMenu();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto win = CCDirector::get()->getWinSize();
        auto ps = m_ps;

        m_panel = CCNode::create();
        m_panel->setContentSize(ps);
        m_panel->setAnchorPoint({0.5f, 1.f});
        m_panel->setPosition({win.width / 2, win.height + ps.height + 20.f});
        this->addChild(m_panel);

        m_bg = CCNode::create();
        m_panel->addChild(m_bg);
        m_border = CCDrawNode::create();
        m_panel->addChild(m_border);

        m_title = CCLabelBMFont::create("Hitbox Menu", "goldFont.fnt");
        m_title->setScale(0.55f);
        m_title->setPosition({ps.width / 2, ps.height - 14.f});
        m_panel->addChild(m_title);

        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({0, 0});
        m_panel->addChild(tabMenu);
        auto& TB = tabs();
        for (int i = 0; i < (int)TB.size(); i++) {
            auto pill = makePill(TB[i].name, 90.f, 28.f, 0.45f, {50, 50, 60}, 220);
            m_tabPills.push_back(pill);
            auto item = CCMenuItemSpriteExtra::create(pill, this, menu_selector(ModMenu::onTab));
            item->setTag(i);
            item->setPosition({54.f, ps.height - 54.f - i * 34.f});
            tabMenu->addChild(item);
        }

        m_content = CCNode::create();
        m_panel->addChild(m_content);

        m_dot = DotNode::create([this]() { this->toggle(); });
        if (m_dot) this->addChild(m_dot, 5);

        applyStyle();
        refresh();
        this->scheduleUpdate();
        return true;
    }

    // ── styling ──
    void recolorTabs() {
        auto ac = accentColor();
        for (int i = 0; i < (int)m_tabPills.size(); i++) {
            auto bg = static_cast<CCScale9Sprite*>(m_tabPills[i]);
            bool sel = i == m_tab;
            bg->setColor(sel ? ac : ccColor3B{50, 50, 60});
            bg->setOpacity(sel ? 255 : 220);
            if (auto l = static_cast<CCLabelBMFont*>(bg->getChildByTag(77)))
                l->setColor(sel ? ccColor3B{0, 0, 0} : ccColor3B{255, 255, 255});
        }
    }

    void drawBorder() {
        m_border->clear();
        int th = (int)getVal("theme", 0);
        if (th != 4 && th != 5) return;
        auto ps = m_ps;
        CCPoint v[4] = {{1, 1}, {ps.width - 1, 1}, {ps.width - 1, ps.height - 1}, {1, ps.height - 1}};
        m_border->drawPolygon(v, 4, {0, 0, 0, 0}, 1.5f, f4(accentColor(), 1.f));
    }

    void applyLight() {
        m_title->setColor(accentColor());
        recolorTabs();
        drawBorder();
        if (m_dot) m_dot->rebuild();
    }

    void applyStyle() {
        auto ps = m_ps;
        m_bg->removeAllChildren();
        int th = std::clamp((int)getVal("theme", 0), 0, 5);
        float op = (float)getVal("menuop", 0.8);
        GLubyte o = (GLubyte)(255.f * op);
        auto pc = PANELS[std::clamp((int)getVal("panelcol", 0), 0, 5)];
        auto ac = accentColor();

        if (th == 5) {
            auto g = CCLayerGradient::create(ccc4(pc.r, pc.g, pc.b, o),
                                             ccc4(ac.r / 3, ac.g / 3, ac.b / 3, o));
            g->setContentSize(ps);
            m_bg->addChild(g);
        } else {
            char const* tex = "square02b_001.png";
            if (th == 1) tex = "GJ_square01.png";
            else if (th == 3 || th == 4) tex = "square02_001.png";
            auto s9 = CCScale9Sprite::create(tex, {0, 0, 80, 80});
            s9->setContentSize(ps);
            s9->setPosition(ps / 2);
            s9->setColor(th == 1 ? ccColor3B{255, 255, 255} : pc);
            s9->setOpacity(th == 2 ? (GLubyte)(o * 0.5f) : o);
            m_bg->addChild(s9);
        }
        applyLight();
    }

    // ── content ──
    void refresh() {
        recolorTabs();
        m_content->removeAllChildren();
        m_valLabels.clear();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_content->addChild(menu);

        auto const& rows = tabs()[m_tab].rows;
        float colW = 212.f, x0 = 108.f;
        for (int i = 0; i < (int)rows.size(); i++) {
            auto const& r = rows[i];
            float cx = x0 + (i / 8) * colW;
            float y = m_ps.height - 54.f - (i % 8) * 28.f;
            m_valLabels.push_back(nullptr);

            if (r.button) {
                auto pill = makePill(r.label, 150.f, 24.f, 0.4f, {70, 70, 85}, 255);
                auto b = CCMenuItemSpriteExtra::create(pill, this, menu_selector(ModMenu::onButton));
                b->setTag(i);
                b->setPosition({cx + 85.f, y});
                menu->addChild(b);
                continue;
            }

            float lx = cx + 4.f;
            if (!r.plain) {
                auto t = CCMenuItemToggler::createWithStandardSprites(
                    this, menu_selector(ModMenu::onToggle), 0.55f);
                t->setTag(i);
                t->toggle(optd(r.key, r.def));
                t->setPosition({cx + 12.f, y});
                menu->addChild(t);
                lx = cx + 30.f;
            }

            auto l = CCLabelBMFont::create(r.label, "bigFont.fnt");
            l->setScale(r.valKey ? 0.3f : 0.38f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({lx, y});
            m_content->addChild(l);

            if (r.valKey) {
                auto mk = [&](char const* str, SEL_MenuHandler cb, float x) {
                    auto lab = CCLabelBMFont::create(str, "goldFont.fnt");
                    lab->setScale(0.8f);
                    auto b = CCMenuItemSpriteExtra::create(lab, this, cb);
                    b->setTag(i);
                    b->setPosition({x, y});
                    menu->addChild(b);
                };
                mk("-", menu_selector(ModMenu::onMinus), cx + 120.f);
                mk("+", menu_selector(ModMenu::onPlus), cx + 198.f);
                auto vl = CCLabelBMFont::create(fmtVal(r, getVal(r.valKey, r.vdef)).c_str(), "bigFont.fnt");
                vl->setScale(0.34f);
                vl->setPosition({cx + 159.f, y});
                m_content->addChild(vl);
                m_valLabels.back() = vl;
            }
        }
    }

    void onTab(CCObject* s) {
        m_tab = static_cast<CCNode*>(s)->getTag();
        refresh();
    }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        auto const& r = tabs()[m_tab].rows[t->getTag()];
        bool on = !t->isToggled(); // callback fires before the visual flips
        setOpt(r.key, on);
        if (std::string(r.key) == "autoldm")
            GameManager::get()->setGameVariable("0042", on);
        if (opt("notify"))
            Notification::create(fmt::format("{}: {}", r.label, on ? "ON" : "OFF"),
                                 NotificationIcon::None, 0.8f)->show();
    }

    void step(CCObject* s, int dir) {
        int i = static_cast<CCNode*>(s)->getTag();
        auto const& r = tabs()[m_tab].rows[i];
        double v = getVal(r.valKey, r.vdef) + dir * r.step;
        v = std::clamp(v, (double)r.vmin, (double)r.vmax);
        v = std::round(v * 10.0) / 10.0;
        setVal(r.valKey, v);
        if (i < (int)m_valLabels.size() && m_valLabels[i])
            m_valLabels[i]->setString(fmtVal(r, v).c_str());
        applyStyle(); // harmless for non-style rows, live for style rows
    }
    void onMinus(CCObject* s) { step(s, -1); }
    void onPlus(CCObject* s) { step(s, +1); }

    void onButton(CCObject* s) {
        auto const& r = tabs()[m_tab].rows[static_cast<CCNode*>(s)->getTag()];
        std::string k = r.key;
        if (k == "resetdot") {
            if (m_dot) m_dot->resetPos(true);
        } else if (k == "resetstyle") {
            setVal("theme", 0); setVal("accent", 0); setVal("panelcol", 0);
            setVal("menuop", 0.8); setVal("dotsize", 18); setVal("dotop", 0.6);
            applyStyle();
            Loader::get()->queueInMainThread([this]() { this->refresh(); });
        }
    }

    void update(float dt) override {
        auto sched = CCDirector::get()->getScheduler();
        bool inLevel = PlayLayer::get() != nullptr;

        float want = 1.f;
        if (inLevel && opt("speedhack")) want = (float)getVal("speed", 1.0);
        if (sched->getTimeScale() != want) sched->setTimeScale(want);

        float scale = sched->getTimeScale();
        if (dt > 0.f) m_fps = m_fps * 0.9f + (scale / dt) * 0.1f;
        s_fps = m_fps;

        s_hue += dt * 0.2f;
        if ((int)getVal("accent", 0) >= 9) applyLight(); // rainbow
    }

    void toggle() {
        m_open = !m_open;
        auto win = CCDirector::get()->getWinSize();
        float y = m_open ? win.height - 6.f : win.height + m_ps.height + 20.f;
        m_panel->stopAllActions();
        auto mv = CCMoveTo::create(0.3f, {win.width / 2, y});
        m_panel->runAction(m_open ? static_cast<CCActionInterval*>(CCEaseBackOut::create(mv))
                                  : static_cast<CCActionInterval*>(CCEaseIn::create(mv, 2.f)));
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
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

// ───────────────────────── small hooks ─────────────────────────
class $modify(GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (down) s_clicks.push_back(nowSec());
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(PlayerObject) {
    void playDeathEffect() {
        if (opt("nodeatheffect")) return;
        PlayerObject::playDeathEffect();
    }
};

class $modify(GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (opt("unlockicons")) return true;
        return GameManager::isIconUnlocked(id, type);
    }
    bool isColorUnlocked(int id, UnlockType type) {
        if (opt("unlockicons")) return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

// ───────────────────────── gameplay / labels ─────────────────────────
class $modify(HMPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* trailNode = nullptr;
        CCDrawNode* hitNode = nullptr;
        std::deque<std::vector<CCRect>> hist;
        std::vector<std::pair<char const*, CCLabelBMFont*>> labels;
        CCLayerColor* flash = nullptr;
        bool cheated = false;
        bool blockedThisFrame = false;
        bool prevBlocked = false;
        int totalFrames = 0;
        int blockedFrames = 0;
        int ncDeaths = 0;
        int attempts = 1;
        float levelTime = 0.f;
        float best = 0.f;
    };

    bool isSafe() {
        return opt("safemode") || (optd("autosafe", true) && m_fields->cheated);
    }

    bool ncActive(PlayerObject* p, GameObject* obj) {
        bool on = opt("noclip") || (opt("noclip_p1") && p == m_player1) ||
                  (opt("noclip_p2") && p == m_player2);
        if (!on) return false;
        if (opt("nc_practice") && !m_isPracticeMode) return false;
        float pct = this->getCurrentPercent();
        if (opt("nc_after") && pct < getVal("ncafter", 0.0)) return false;
        if (opt("nc_until") && pct > getVal("ncuntil", 100.0)) return false;
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
        PlayLayer::destroyPlayer(p, obj);
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        if (opt("autopractice") && !m_isPracticeMode) this->togglePracticeMode(true);
    }

    void resetLevel() {
        auto f = m_fields.self();
        f->hist.clear();
        f->attempts++;
        f->levelTime = 0.f;
        f->totalFrames = 0;
        f->blockedFrames = 0;
        f->ncDeaths = 0;
        f->prevBlocked = false;
        PlayLayer::resetLevel();
    }

    // Safe mode: completions/progress are not saved once cheats were used
    void levelComplete() {
        if (isSafe()) m_isTestMode = true;
        PlayLayer::levelComplete();
    }
    void onQuit() {
        if (isSafe()) m_isTestMode = true;
        PlayLayer::onQuit();
    }

    void makeLabels() {
        auto f = m_fields.self();
        auto w = CCDirector::get()->getWinSize();
        f->flash = CCLayerColor::create({255, 0, 0, 0}, w.width, w.height);
        m_uiLayer->addChild(f->flash, 40);
        for (auto k : {"lbl_fps", "lbl_pct", "lbl_attempt", "lbl_time", "lbl_best",
                       "lbl_clock", "lbl_cps", "lbl_nca", "lbl_ncd", "lbl_test"}) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setAnchorPoint({0.f, 1.f});
            m_uiLayer->addChild(l, 50);
            f->labels.push_back({k, l});
        }
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

        float sc = 0.32f * (opt("lbl_size") ? (float)getVal("lblscale", 1.0) : 1.f);
        bool master = optd("lbl_master", true);
        auto lc = colIdx("lblcol", 8);
        GLubyte lo = (GLubyte)(255.0 * getVal("lblop", 0.8));
        float y = win.height - 6.f;
        for (auto& [key, lab] : f->labels) {
            std::string k = key;
            bool show = master && opt(key);
            std::string txt;
            if (k == "lbl_fps") txt = fmt::format("FPS: {}", (int)(s_fps + 0.5f));
            else if (k == "lbl_pct")
                txt = opt("lbl_acc") ? fmt::format("{:.2f}%", pct) : fmt::format("{}%", (int)pct);
            else if (k == "lbl_attempt") txt = fmt::format("Attempt {}", f->attempts);
            else if (k == "lbl_time") {
                int m = (int)(f->levelTime / 60.f);
                txt = fmt::format("{}:{:05.2f}", m, f->levelTime - m * 60.f);
            }
            else if (k == "lbl_best") txt = fmt::format("Best: {:.1f}%", f->best);
            else if (k == "lbl_clock") {
                std::time_t t = std::time(nullptr);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
                txt = buf;
            }
            else if (k == "lbl_cps") txt = fmt::format("CPS: {}", s_clicks.size());
            else if (k == "lbl_nca") txt = fmt::format("Noclip Accuracy: {:.2f}%", accuracy());
            else if (k == "lbl_ncd") txt = fmt::format("Noclip Deaths: {}", f->ncDeaths);
            else if (k == "lbl_test") {
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

        if (!f->trailNode) {
            f->trailNode = CCDrawNode::create();
            m_objectLayer->addChild(f->trailNode, 1000);
            f->hitNode = CCDrawNode::create();
            m_objectLayer->addChild(f->hitNode, 1001);
        }

        // read the flag BEFORE resetting it (used by flash / hitbox colour)
        bool blocked = f->blockedThisFrame;

        if (opt("noclip") || opt("noclip_p1") || opt("noclip_p2") || opt("jumphack") ||
            (opt("speedhack") && std::fabs(getVal("speed", 1.0) - 1.0) > 0.001))
            f->cheated = true;

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
                PlayLayer::destroyPlayer(m_player1, m_anticheatSpike);
        }

        if (m_player1 && !m_player1->m_isDead) f->levelTime += dt;
        float pct = this->getCurrentPercent();
        if (pct > f->best) f->best = pct;

        if (opt("jumphack")) {
            if (m_player1) m_player1->m_isOnGround = true;
            if (m_player2) m_player2->m_isOnGround = true;
        }

        bool hide = opt("hideplayer");
        if (m_player1) m_player1->setVisible(!hide);
        if (m_player2) m_player2->setVisible(!hide);

        updateLabels(pct, blocked);

        float thick = (float)getVal("hbthick", 1.0);

        // ── hitboxes (drawn manually) ──
        f->hitNode->clear();
        bool showHb = opt("hitbox") || (opt("hitdeath") && m_player1 && m_player1->m_isDead);
        if (showHb && m_player1) {
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
