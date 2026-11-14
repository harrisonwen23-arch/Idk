#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
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
static std::deque<double> s_clicks;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void drawRect(CCDrawNode* n, CCRect const& rc, ccColor4F col, bool fill) {
    CCPoint v[4] = {
        {rc.getMinX(), rc.getMinY()}, {rc.getMaxX(), rc.getMinY()},
        {rc.getMaxX(), rc.getMaxY()}, {rc.getMinX(), rc.getMaxY()}
    };
    ccColor4F fc = fill ? ccColor4F{col.r, col.g, col.b, col.a * 0.25f} : ccColor4F{0, 0, 0, 0};
    n->drawPolygon(v, 4, fc, 0.8f, col);
}

// ───────────────────────── menu definition ─────────────────────────
struct Row {
    char const* key;
    char const* label;
    bool def = false;            // default toggle state
    char const* valKey = nullptr; // if set, row has a - / + stepper
    float vmin = 0, vmax = 0, step = 0, vdef = 0;
};
struct Tab { char const* name; std::vector<Row> rows; };

static std::vector<Tab> const& tabs() {
    static std::vector<Tab> t = {
        {"Global", {
            {"speedhack", "Speedhack", false, "speed", 0.1f, 5.f, 0.1f, 1.f},
            {"safemode", "Safe Mode"},
            {"autosafe", "Auto Safe Mode", true},
            {"unlockicons", "Unlock Icons"},
        }},
        {"Level", {
            {"hitbox", "Show Hitboxes"},
            {"hitdeath", "Hitboxes On Death"},
            {"trail", "Hitbox Trail"},
            {"fill", "Fill Hitboxes"},
            {"autoldm", "Auto LDM"},
        }},
        {"Player", {
            {"noclip", "Noclip"},
            {"jumphack", "Jump Hack"},
            {"nodeatheffect", "No Death Effect"},
            {"hideplayer", "Hide Player"},
        }},
        {"Labels", {
            {"lbl_fps", "FPS"},
            {"lbl_pct", "Percentage"},
            {"lbl_acc", "Accurate Percentage"},
            {"lbl_attempt", "Attempt"},
            {"lbl_time", "Level Time"},
            {"lbl_best", "Best Run"},
            {"lbl_clock", "Clock"},
            {"lbl_cps", "CPS"},
            {"lbl_nca", "Noclip Accuracy"},
            {"lbl_ncd", "Noclip Deaths"},
            {"lbl_test", "Testmode / Safe Mode"},
        }},
    };
    return t;
}

class ModMenu : public CCNode {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    ScrollLayer* m_scroll = nullptr;
    std::vector<ButtonSprite*> m_tabSprites;
    std::vector<CCLabelBMFont*> m_valLabels;
    int m_tab = 0;
    bool m_open = false;
    float m_fps = 60.f;
    CCSize m_ps {370.f, 235.f};

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

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setContentSize(ps);
        bg->setColor({0, 0, 0});
        bg->setOpacity(215);
        bg->setPosition(ps / 2);
        m_panel->addChild(bg);

        auto title = CCLabelBMFont::create("Hitbox Menu", "goldFont.fnt");
        title->setScale(0.55f);
        title->setPosition({ps.width / 2, ps.height - 14.f});
        m_panel->addChild(title);

        // vertical tab buttons on the left
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({0, 0});
        m_panel->addChild(tabMenu);
        auto& T = tabs();
        for (int i = 0; i < (int)T.size(); i++) {
            auto spr = ButtonSprite::create(T[i].name, 80, true, "bigFont.fnt", "GJ_button_01.png", 26.f, 0.6f);
            m_tabSprites.push_back(spr);
            auto item = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ModMenu::onTab));
            item->setTag(i);
            item->setPosition({54.f, ps.height - 50.f - i * 36.f});
            tabMenu->addChild(item);
        }

        refresh();

        // small transparent dot with logo (top-left)
        CCNode* dot = CCSprite::create("dot-logo.png"_spr);
        float d = 36.f;
        if (dot) {
            dot->setScale(d / dot->getContentSize().width);
            static_cast<CCSprite*>(dot)->setOpacity(150);
        } else {
            auto dn = CCDrawNode::create();
            dn->drawDot({d / 2, d / 2}, d / 2, {0.f, 0.85f, 1.f, 0.55f});
            dn->setContentSize({d, d});
            dot = dn;
        }
        auto btn = CCMenuItemSpriteExtra::create(dot, this, menu_selector(ModMenu::onDot));
        auto dm = CCMenu::create();
        dm->addChild(btn);
        dm->setPosition({30.f, win.height - 30.f});
        this->addChild(dm, 5);

        this->scheduleUpdate();
        return true;
    }

    void refresh() {
        for (int i = 0; i < (int)m_tabSprites.size(); i++)
            m_tabSprites[i]->updateBGImage(i == m_tab ? "GJ_button_02.png" : "GJ_button_01.png");

        if (m_scroll) { m_scroll->removeFromParent(); m_scroll = nullptr; }
        m_valLabels.clear();

        auto const& rows = tabs()[m_tab].rows;
        CCSize view {258.f, 190.f};
        float rowH = 28.f;
        float H = std::max(view.height, rows.size() * rowH + 8.f);

        m_scroll = ScrollLayer::create(view);
        m_scroll->setPosition({104.f, 10.f});
        m_panel->addChild(m_scroll);
        m_scroll->m_contentLayer->setContentSize({view.width, H});

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        menu->setContentSize({view.width, H});
        m_scroll->m_contentLayer->addChild(menu);

        for (int i = 0; i < (int)rows.size(); i++) {
            auto const& r = rows[i];
            float y = H - 16.f - i * rowH;

            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(ModMenu::onToggle), 0.55f);
            t->setTag(i);
            t->toggle(optd(r.key, r.def));
            t->setPosition({16.f, y});
            menu->addChild(t);

            auto l = CCLabelBMFont::create(r.label, "bigFont.fnt");
            l->setScale(0.4f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({34.f, y});
            m_scroll->m_contentLayer->addChild(l);

            CCLabelBMFont* vl = nullptr;
            if (r.valKey) {
                auto mk = [&](char const* s, SEL_MenuHandler cb, float x) {
                    auto lab = CCLabelBMFont::create(s, "goldFont.fnt");
                    lab->setScale(0.8f);
                    auto b = CCMenuItemSpriteExtra::create(lab, this, cb);
                    b->setTag(i);
                    b->setPosition({x, y});
                    menu->addChild(b);
                };
                mk("-", menu_selector(ModMenu::onMinus), view.width - 74.f);
                mk("+", menu_selector(ModMenu::onPlus), view.width - 16.f);
                vl = CCLabelBMFont::create(fmt::format("{:.1f}", getVal(r.valKey, r.vdef)).c_str(), "bigFont.fnt");
                vl->setScale(0.4f);
                vl->setPosition({view.width - 45.f, y});
                m_scroll->m_contentLayer->addChild(vl);
            }
            m_valLabels.push_back(vl);
        }
        m_scroll->moveToTop();
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
    }

    void step(CCObject* s, int dir) {
        int i = static_cast<CCNode*>(s)->getTag();
        auto const& r = tabs()[m_tab].rows[i];
        double v = getVal(r.valKey, r.vdef) + dir * r.step;
        v = std::clamp(v, (double)r.vmin, (double)r.vmax);
        v = std::round(v * 10.0) / 10.0;
        setVal(r.valKey, v);
        if (i < (int)m_valLabels.size() && m_valLabels[i])
            m_valLabels[i]->setString(fmt::format("{:.1f}", v).c_str());
    }
    void onMinus(CCObject* s) { step(s, -1); }
    void onPlus(CCObject* s) { step(s, +1); }

    void onDot(CCObject*) { toggle(); }

    void update(float dt) override {
        auto sched = CCDirector::get()->getScheduler();
        bool inLevel = PlayLayer::get() != nullptr;

        float want = 1.f;
        if (inLevel && opt("speedhack")) want = (float)getVal("speed", 1.0);
        if (sched->getTimeScale() != want) sched->setTimeScale(want);

        float scale = sched->getTimeScale();
        if (dt > 0.f) m_fps = m_fps * 0.9f + (scale / dt) * 0.1f;
        s_fps = m_fps;
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

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (opt("noclip") && obj != m_anticheatSpike) {
            m_fields->blockedThisFrame = true;
            return;
        }
        PlayLayer::destroyPlayer(p, obj);
    }

    void resetLevel() {
        m_fields->hist.clear();
        m_fields->attempts++;
        m_fields->levelTime = 0.f;
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
        for (auto k : {"lbl_fps", "lbl_pct", "lbl_attempt", "lbl_time", "lbl_best",
                       "lbl_clock", "lbl_cps", "lbl_nca", "lbl_ncd", "lbl_test"}) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setScale(0.32f);
            l->setAnchorPoint({0.f, 1.f});
            l->setOpacity(200);
            m_uiLayer->addChild(l, 50);
            f->labels.push_back({k, l});
        }
    }

    void updateLabels(float dt, float pct) {
        auto f = m_fields.self();
        if (f->labels.empty()) makeLabels();
        auto win = CCDirector::get()->getWinSize();

        double now = nowSec();
        while (!s_clicks.empty() && now - s_clicks.front() > 1.0) s_clicks.pop_front();

        float y = win.height - 6.f;
        for (auto& [key, lab] : f->labels) {
            std::string k = key;
            bool show = opt(key);
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
            else if (k == "lbl_nca") {
                float acc = f->totalFrames > 0
                    ? 100.f * (1.f - float(f->blockedFrames) / float(f->totalFrames)) : 100.f;
                txt = fmt::format("Noclip Accuracy: {:.2f}%", acc);
            }
            else if (k == "lbl_ncd") txt = fmt::format("Noclip Deaths: {}", f->ncDeaths);
            else if (k == "lbl_test") {
                if (isSafe()) txt = "Safe Mode";
                else if (m_isTestMode) txt = "Testmode";
                else show = false;
            }
            lab->setVisible(show);
            if (!show) continue;
            lab->setString(txt.c_str());
            lab->setPosition({62.f, y});
            y -= 11.f;
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

        // cheat tracking (for Safe Mode)
        if (opt("noclip") || opt("jumphack") ||
            (opt("speedhack") && std::fabs(getVal("speed", 1.0) - 1.0) > 0.001))
            f->cheated = true;

        // noclip stats
        f->totalFrames++;
        if (f->blockedThisFrame) {
            f->blockedFrames++;
            if (!f->prevBlocked) f->ncDeaths++;
        }
        f->prevBlocked = f->blockedThisFrame;
        f->blockedThisFrame = false;

        if (m_player1 && !m_player1->m_isDead) f->levelTime += dt;
        float pct = this->getCurrentPercent();
        if (pct > f->best) f->best = pct;

        // jump hack
        if (opt("jumphack")) {
            if (m_player1) m_player1->m_isOnGround = true;
            if (m_player2) m_player2->m_isOnGround = true;
        }

        bool hide = opt("hideplayer");
        if (m_player1) m_player1->setVisible(!hide);
        if (m_player2) m_player2->setVisible(!hide);

        updateLabels(dt, pct);

        // ── hitboxes (drawn manually) ──
        f->hitNode->clear();
        bool showHb = opt("hitbox") || (opt("hitdeath") && m_player1 && m_player1->m_isDead);
        if (showHb && m_player1) {
            bool fill = opt("fill");
            float px = m_player1->getPositionX();
            for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
                if (!obj) continue;
                float dx = obj->getPositionX() - px;
                if (dx < -250.f || dx > 450.f) continue;
                ccColor4F col;
                if (obj->m_objectType == GameObjectType::Hazard) col = {1.f, 0.f, 0.f, 1.f};
                else if (obj->m_objectType == GameObjectType::Solid) col = {0.f, 0.4f, 1.f, 1.f};
                else continue;
                drawRect(f->hitNode, obj->getObjectRect(), col, fill);
            }
            drawRect(f->hitNode, m_player1->getObjectRect(), {1.f, 0.9f, 0.f, 1.f}, fill);
            if (m_gameState.m_isDualMode && m_player2)
                drawRect(f->hitNode, m_player2->getObjectRect(), {1.f, 0.9f, 0.f, 1.f}, fill);
        }

        // ── trail ──
        f->trailNode->clear();
        if (!opt("trail")) { f->hist.clear(); return; }

        if (m_player1 && !m_player1->m_isDead) {
            std::vector<CCRect> r { m_player1->getObjectRect() };
            if (m_gameState.m_isDualMode && m_player2) r.push_back(m_player2->getObjectRect());
            f->hist.push_back(std::move(r));
            while (f->hist.size() > TRAIL_LEN) f->hist.pop_front();
        }

        size_t n = f->hist.size();
        for (size_t i = 0; i < n; i += TRAIL_STEP) {
            float a = 0.15f + 0.8f * (float(i + 1) / float(n));
            for (size_t j = 0; j < f->hist[i].size(); j++) {
                ccColor4F col = j == 0 ? ccColor4F{1.f, 0.2f, 0.2f, a} : ccColor4F{0.2f, 0.5f, 1.f, a};
                drawRect(f->trailNode, f->hist[i][j], col, false);
            }
        }
    }
};
