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
#include <functional>
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
            {"fill", "Fill Hitboxes"},
            {"trail", "Hitbox Trail"},
            {"customtrail", "Trail Length", false, "traillen", 30.f, 600.f, 30.f, 240.f},
            {"autopractice", "Auto Practice Mode"},
            {"autoldm", "Auto LDM"},
        }},
        {"Player", {
            {"noclip", "Noclip"},
            {"noclip_p1", "Noclip P1 Only"},
            {"noclip_p2", "Noclip P2 Only"},
            {"ncflash", "Noclip Flash"},
            {"jumphack", "Jump Hack"},
            {"nodeatheffect", "No Death Effect"},
            {"hideplayer", "Hide Player"},
        }},
        {"Labels", {
            {"lbl_master", "Show Labels", true},
            {"lbl_size", "Label Size", false, "lblscale", 0.5f, 2.f, 0.1f, 1.f},
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

static std::string fmtVal(Row const& r, double v) {
    return r.step >= 1.f ? fmt::format("{:.0f}", v) : fmt::format("{:.1f}", v);
}

// Draggable dot: hold and drag to move, tap to open/close the menu
class DotNode : public CCNode, public CCTouchDelegate {
public:
    std::function<void()> m_onTap;
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
        auto d = CCDrawNode::create();
        d->drawDot({0, 0}, m_r, {0.f, 0.85f, 1.f, 0.45f});
        d->drawDot({0, 0}, m_r - 2.f, {0.04f, 0.04f, 0.08f, 0.55f});
        CCPoint sq[4] = {{-8, -8}, {8, -8}, {8, 8}, {-8, 8}};
        d->drawPolygon(sq, 4, {0, 0, 0, 0}, 1.3f, {1.f, 0.24f, 0.24f, 0.8f});
        CCPoint in[4] = {{-3.5f, -3.5f}, {3.5f, -3.5f}, {3.5f, 3.5f}, {-3.5f, 3.5f}};
        d->drawPolygon(in, 4, {0.f, 0.86f, 1.f, 0.8f}, 0.f, {0, 0, 0, 0});
        this->addChild(d);

        auto win = CCDirector::get()->getWinSize();
        this->setPosition({(float)getVal("dotx", 30.0), (float)getVal("doty", win.height - 30.0)});
        CCTouchDispatcher::get()->addTargetedDelegate(this, -600, true);
    }

    ~DotNode() override { CCTouchDispatcher::get()->removeDelegate(this); }

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

class ModMenu : public CCNode {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    CCNode* m_content = nullptr;
    std::vector<ButtonSprite*> m_tabSprites;
    std::vector<CCLabelBMFont*> m_valLabels;
    int m_tab = 0;
    bool m_open = false;
    float m_fps = 60.f;
    CCSize m_ps {520.f, 240.f};

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
            item->setPosition({54.f, ps.height - 52.f - i * 38.f});
            tabMenu->addChild(item);
        }

        m_content = CCNode::create();
        m_panel->addChild(m_content);
        refresh();

        auto dot = DotNode::create([this]() { this->toggle(); });
        if (dot) this->addChild(dot, 5);

        this->scheduleUpdate();
        return true;
    }

    void refresh() {
        for (int i = 0; i < (int)m_tabSprites.size(); i++)
            m_tabSprites[i]->updateBGImage(i == m_tab ? "GJ_button_02.png" : "GJ_button_01.png");

        m_content->removeAllChildren();
        m_valLabels.clear();

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_content->addChild(menu);

        auto const& rows = tabs()[m_tab].rows;
        float colW = 205.f, x0 = 108.f;
        for (int i = 0; i < (int)rows.size(); i++) {
            auto const& r = rows[i];
            float cx = x0 + (i / 7) * colW;
            float y = m_ps.height - 52.f - (i % 7) * 28.f;

            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(ModMenu::onToggle), 0.55f);
            t->setTag(i);
            t->toggle(optd(r.key, r.def));
            t->setPosition({cx + 12.f, y});
            menu->addChild(t);

            auto l = CCLabelBMFont::create(r.label, "bigFont.fnt");
            l->setScale(0.38f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({cx + 30.f, y});
            m_content->addChild(l);

            CCLabelBMFont* vl = nullptr;
            if (r.valKey) {
                auto mk = [&](char const* str, SEL_MenuHandler cb, float x) {
                    auto lab = CCLabelBMFont::create(str, "goldFont.fnt");
                    lab->setScale(0.8f);
                    auto b = CCMenuItemSpriteExtra::create(lab, this, cb);
                    b->setTag(i);
                    b->setPosition({x, y});
                    menu->addChild(b);
                };
                // stepper sits on the line below the label so long names fit
                mk("-", menu_selector(ModMenu::onMinus), cx + 130.f);
                mk("+", menu_selector(ModMenu::onPlus), cx + 190.f);
                vl = CCLabelBMFont::create(fmtVal(r, getVal(r.valKey, r.vdef)).c_str(), "bigFont.fnt");
                vl->setScale(0.38f);
                vl->setPosition({cx + 160.f, y});
                m_content->addChild(vl);
                l->setPosition({cx + 30.f, y + 0.f});
                l->setScale(0.3f);
            }
            m_valLabels.push_back(vl);
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
    }
    void onMinus(CCObject* s) { step(s, -1); }
    void onPlus(CCObject* s) { step(s, +1); }

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
        CCLayerColor* flash = nullptr;
    };

    bool isSafe() {
        return opt("safemode") || (optd("autosafe", true) && m_fields->cheated);
    }

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        bool nc = opt("noclip") || (opt("noclip_p1") && p == m_player1) ||
                  (opt("noclip_p2") && p == m_player2);
        if (nc && obj != m_anticheatSpike) {
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
        auto w = CCDirector::get()->getWinSize();
        f->flash = CCLayerColor::create({255, 0, 0, 0}, w.width, w.height);
        m_uiLayer->addChild(f->flash, 40);
        for (auto k : {"lbl_fps", "lbl_pct", "lbl_attempt", "lbl_time", "lbl_best",
                       "lbl_clock", "lbl_cps", "lbl_nca", "lbl_ncd", "lbl_test"}) {
            auto l = CCLabelBMFont::create("", "bigFont.fnt");
            l->setScale(0.32f);
            l->setTag(1);
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

        if (f->flash) {
            int o = f->flash->getOpacity();
            if (opt("ncflash") && f->blockedThisFrame) o = 90;
            else o = std::max(0, o - 12);
            f->flash->setOpacity(o);
        }

        float sc = 0.32f * (opt("lbl_size") ? (float)getVal("lblscale", 1.0) : 1.f);
        bool master = optd("lbl_master", true);
        float y = win.height - 6.f;
        for (auto& [key, lab] : f->labels) {
            std::string k = key;
            bool show = master && opt(key);
            lab->setScale(sc);
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

        // cheat tracking (for Safe Mode)
        if (opt("noclip") || opt("noclip_p1") || opt("noclip_p2") || opt("jumphack") ||
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
            size_t maxLen = opt("customtrail") ? (size_t)getVal("traillen", 240.0) : TRAIL_LEN;
            while (f->hist.size() > maxLen) f->hist.pop_front();
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
