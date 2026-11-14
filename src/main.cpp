#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/OverlayManager.hpp>
#include <deque>
#include <vector>

using namespace geode::prelude;

static bool opt(char const* k) { return Mod::get()->getSavedValue<bool>(k, false); }
static void setOpt(char const* k, bool v) { Mod::get()->setSavedValue<bool>(k, v); }

static constexpr size_t TRAIL_LEN = 240; // frames kept
static constexpr size_t TRAIL_STEP = 3;  // draw every Nth frame

static void drawRect(CCDrawNode* n, CCRect const& rc, ccColor4F col, bool fill) {
    CCPoint v[4] = {
        {rc.getMinX(), rc.getMinY()}, {rc.getMaxX(), rc.getMinY()},
        {rc.getMaxX(), rc.getMaxY()}, {rc.getMinX(), rc.getMaxY()}
    };
    ccColor4F fc = fill ? ccColor4F{col.r, col.g, col.b, col.a * 0.25f} : ccColor4F{0, 0, 0, 0};
    n->drawPolygon(v, 4, fc, 0.8f, col);
}

// ───────────────────────── Menu ─────────────────────────
struct Row { char const* key; char const* label; };
struct Tab { char const* name; std::vector<Row> rows; };

static std::vector<Tab> const& tabs() {
    static std::vector<Tab> t = {
        {"Gameplay", {{"noclip", "Noclip"}, {"slowmo", "Slow Motion"}}},
        {"Hitbox",   {{"hitbox", "Show Hitboxes"}, {"trail", "Hitbox Trail"}, {"fill", "Fill Hitboxes"}}},
        {"Visual",   {{"hideplayer", "Hide Player"}, {"fps", "Show FPS"}}},
    };
    return t;
}

class ModMenu : public CCNode {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    CCNode* m_content = nullptr;
    CCLabelBMFont* m_fpsLabel = nullptr;
    std::vector<ButtonSprite*> m_tabSprites;
    int m_tab = 0;
    bool m_open = false;
    float m_fpsTimer = 0.f;
    float m_fps = 60.f;
    CCSize m_ps {250.f, 175.f};

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
        bg->setOpacity(200);
        bg->setPosition(ps / 2);
        m_panel->addChild(bg);

        auto title = CCLabelBMFont::create("Hitbox Menu", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition({ps.width / 2, ps.height - 16.f});
        m_panel->addChild(title);

        // tab buttons
        auto tabMenu = CCMenu::create();
        tabMenu->setPosition({0, 0});
        m_panel->addChild(tabMenu);
        auto& T = tabs();
        for (int i = 0; i < (int)T.size(); i++) {
            auto spr = ButtonSprite::create(T[i].name, 70, true, "bigFont.fnt", "GJ_button_01.png", 24.f, 0.7f);
            m_tabSprites.push_back(spr);
            auto item = CCMenuItemSpriteExtra::create(spr, this, menu_selector(ModMenu::onTab));
            item->setTag(i);
            item->setPosition({ps.width / 2 + (i - 1) * 78.f, ps.height - 44.f});
            tabMenu->addChild(item);
        }

        m_content = CCNode::create();
        m_panel->addChild(m_content);
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

        // FPS label
        m_fpsLabel = CCLabelBMFont::create("", "bigFont.fnt");
        m_fpsLabel->setScale(0.4f);
        m_fpsLabel->setAnchorPoint({1.f, 0.5f});
        m_fpsLabel->setPosition({win.width - 8.f, 12.f});
        m_fpsLabel->setVisible(false);
        this->addChild(m_fpsLabel, 3);

        this->scheduleUpdate();
        return true;
    }

    void refresh() {
        for (int i = 0; i < (int)m_tabSprites.size(); i++)
            m_tabSprites[i]->updateBGImage(i == m_tab ? "GJ_button_02.png" : "GJ_button_01.png");

        m_content->removeAllChildren();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_content->addChild(menu);

        auto const& rows = tabs()[m_tab].rows;
        for (int i = 0; i < (int)rows.size(); i++) {
            float y = m_ps.height - 88.f - i * 30.f;
            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(ModMenu::onToggle), 0.65f);
            t->setTag(i);
            t->toggle(opt(rows[i].key));
            t->setPosition({m_ps.width - 32.f, y});
            menu->addChild(t);

            auto l = CCLabelBMFont::create(rows[i].label, "bigFont.fnt");
            l->setScale(0.5f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({18.f, y});
            m_content->addChild(l);
        }
    }

    void onTab(CCObject* s) {
        m_tab = static_cast<CCNode*>(s)->getTag();
        refresh();
    }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        // callback fires before the visual state flips
        setOpt(tabs()[m_tab].rows[t->getTag()].key, !t->isToggled());
    }

    void onDot(CCObject*) { toggle(); }

    void update(float dt) override {
        auto sched = CCDirector::get()->getScheduler();
        bool inLevel = PlayLayer::get() != nullptr;

        // slow motion only while inside a level
        float want = (inLevel && opt("slowmo")) ? 0.5f : 1.f;
        if (sched->getTimeScale() != want) sched->setTimeScale(want);

        // fps counter
        float scale = sched->getTimeScale();
        if (dt > 0.f) m_fps = m_fps * 0.9f + (scale / dt) * 0.1f;
        m_fpsTimer += dt;
        bool show = inLevel && opt("fps");
        m_fpsLabel->setVisible(show);
        if (show && m_fpsTimer > 0.25f * scale) {
            m_fpsTimer = 0.f;
            m_fpsLabel->setString(fmt::format("FPS: {}", (int)(m_fps + 0.5f)).c_str());
        }
    }

    void toggle() {
        m_open = !m_open;
        auto win = CCDirector::get()->getWinSize();
        float y = m_open ? win.height - 8.f : win.height + m_ps.height + 20.f;
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

// Tab opens/closes the menu (desktop)
class $modify(CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double ts) {
        if (down && !repeat && key == KEY_Tab && ModMenu::s_inst) {
            ModMenu::s_inst->toggle();
            return true;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, ts);
    }
};

// ───────────────────────── Hacks ─────────────────────────
class $modify(HMPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* trailNode = nullptr;
        CCDrawNode* hitNode = nullptr;
        std::deque<std::vector<CCRect>> hist;
    };

    void destroyPlayer(PlayerObject* p, GameObject* obj) {
        if (opt("noclip") && obj != m_anticheatSpike) return;
        PlayLayer::destroyPlayer(p, obj);
    }

    void resetLevel() {
        m_fields->hist.clear();
        PlayLayer::resetLevel();
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

        bool hide = opt("hideplayer");
        if (m_player1) m_player1->setVisible(!hide);
        if (m_player2) m_player2->setVisible(!hide);

        // ── hitboxes (drawn manually) ──
        f->hitNode->clear();
        if (opt("hitbox") && m_player1) {
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
