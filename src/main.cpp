#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <deque>
#include <vector>

using namespace geode::prelude;

static bool opt(char const* k) { return Mod::get()->getSavedValue<bool>(k, false); }
static void setOpt(char const* k, bool v) { Mod::get()->setSavedValue<bool>(k, v); }

static constexpr size_t TRAIL_LEN = 150; // frames

// ───────────────────────── Menu ─────────────────────────
class ModMenu : public CCNode {
public:
    static inline ModMenu* s_inst = nullptr;
    CCNode* m_panel = nullptr;
    bool m_open = false;

    struct Row { char const* key; char const* label; };
    static constexpr Row ROWS[3] = {
        {"noclip",  "Noclip"},
        {"hitbox",  "Show Hitboxes"},
        {"trail",   "Hitbox Trail"},
    };

    static ModMenu* create() {
        auto r = new ModMenu();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto win = CCDirector::get()->getWinSize();
        CCSize ps {200.f, 130.f};

        m_panel = CCNode::create();
        m_panel->setContentSize(ps);
        m_panel->setAnchorPoint({0.5f, 1.f});
        m_panel->setPosition({win.width / 2, win.height + ps.height + 20.f});
        this->addChild(m_panel);

        auto bg = CCScale9Sprite::create("square02b_001.png", {0, 0, 80, 80});
        bg->setContentSize(ps);
        bg->setColor({0, 0, 0});
        bg->setOpacity(190);
        bg->setPosition(ps / 2);
        m_panel->addChild(bg);

        auto title = CCLabelBMFont::create("Hitbox Menu", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition({ps.width / 2, ps.height - 18.f});
        m_panel->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_panel->addChild(menu);

        for (int i = 0; i < 3; i++) {
            float y = ps.height - 52.f - i * 34.f;
            auto t = CCMenuItemToggler::createWithStandardSprites(
                this, menu_selector(ModMenu::onToggle), 0.7f);
            t->setTag(i);
            t->toggle(opt(ROWS[i].key));
            t->setPosition({ps.width - 32.f, y});
            menu->addChild(t);

            auto l = CCLabelBMFont::create(ROWS[i].label, "bigFont.fnt");
            l->setScale(0.5f);
            l->setAnchorPoint({0.f, 0.5f});
            l->setPosition({18.f, y});
            m_panel->addChild(l);
        }

#ifdef GEODE_IS_MOBILE
        // small transparent dot with logo
        auto dot = CCSprite::create("dot-logo.png"_spr);
        if (dot) {
            dot->setScale(24.f / dot->getContentSize().width);
            dot->setOpacity(100);
            auto btn = CCMenuItemSpriteExtra::create(dot, this, menu_selector(ModMenu::onDot));
            auto dm = CCMenu::create();
            dm->addChild(btn);
            dm->setPosition({20.f, win.height - 20.f});
            this->addChild(dm, 5);
        }
#endif
        return true;
    }

    void onToggle(CCObject* s) {
        auto t = static_cast<CCMenuItemToggler*>(s);
        // callback fires before the visual state flips
        setOpt(ROWS[t->getTag()].key, !t->isToggled());
    }

    void onDot(CCObject*) { toggle(); }

    void toggle() {
        m_open = !m_open;
        auto win = CCDirector::get()->getWinSize();
        float y = m_open ? win.height - 8.f : win.height + m_panel->getContentSize().height + 20.f;
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
                SceneManager::get()->keepAcrossScenes(ModMenu::s_inst);
            }
        }
        return true;
    }
};

// Tab opens/closes the menu
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
        CCDrawNode* node = nullptr;
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
        if (!f->node) {
            f->node = CCDrawNode::create();
            m_objectLayer->addChild(f->node, 1000);
        }
        f->node->clear();
        if (!opt("trail")) { f->hist.clear(); return; }

        if (m_player1 && !m_player1->m_isDead) {
            std::vector<CCRect> r { m_player1->getObjectRect() };
            if (m_gameState.m_isDualMode && m_player2) r.push_back(m_player2->getObjectRect());
            f->hist.push_back(std::move(r));
            while (f->hist.size() > TRAIL_LEN) f->hist.pop_front();
        }

        size_t n = f->hist.size();
        for (size_t i = 0; i < n; i++) {
            float a = 0.15f + 0.75f * (float(i + 1) / float(n));
            for (size_t j = 0; j < f->hist[i].size(); j++) {
                auto const& rc = f->hist[i][j];
                CCPoint v[4] = {
                    {rc.getMinX(), rc.getMinY()}, {rc.getMaxX(), rc.getMinY()},
                    {rc.getMaxX(), rc.getMaxY()}, {rc.getMinX(), rc.getMaxY()}
                };
                ccColor4F col = j == 0 ? ccColor4F{1.f, 0.2f, 0.2f, a} : ccColor4F{0.2f, 0.5f, 1.f, a};
                f->node->drawPolygon(v, 4, {0, 0, 0, 0}, 0.6f, col);
            }
        }
    }
};

class $modify(GJBaseGameLayer) {
    void updateDebugDraw() {
        auto gm = GameManager::get();
        bool prev = gm->getGameVariable("0166");
        bool on = opt("hitbox");
        if (on) gm->setGameVariable("0166", true);
        GJBaseGameLayer::updateDebugDraw();
        if (on) gm->setGameVariable("0166", prev);
    }
};
