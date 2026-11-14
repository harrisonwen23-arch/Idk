#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>

using namespace geode::prelude;

// Forward declaration of the Mod Menu Popup
class ModMenuPopup : public Popup<> {
protected:
    bool setup() override {
        this->setTitle("Mod Menu");
        auto winSize = this->m_size;

        auto menu = CCMenu::create();
        menu->setLayout(ColumnLayout::create()->setGap(10.f));
        menu->setPosition({winSize.width / 2, winSize.height / 2 - 10.f});

        // 1. Noclip Toggle
        auto noclipToggle = CCMenuItemToggler::create(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            this,
            menu_selector(ModMenuPopup::onToggleNoclip)
        );
        noclipToggle->toggle(Mod::get()->getSettingValue<bool>("noclip"));
        
        auto noclipLabel = CCLabelBMFont::create("Noclip", "bigFont.fnt");
        noclipLabel->setScale(0.4f);

        // 2. Hitboxes Toggle
        auto hitboxToggle = CCMenuItemToggler::create(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            this,
            menu_selector(ModMenuPopup::onToggleHitboxes)
        );
        hitboxToggle->toggle(Mod::get()->getSettingValue<bool>("hitboxes"));

        auto hitboxLabel = CCLabelBMFont::create("Show Hitboxes", "bigFont.fnt");
        hitboxLabel->setScale(0.4f);

        // 3. Hitbox Trail Toggle
        auto trailToggle = CCMenuItemToggler::create(
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            this,
            menu_selector(ModMenuPopup::onToggleTrail)
        );
        trailToggle->toggle(Mod::get()->getSettingValue<bool>("hitbox-trail"));

        auto trailLabel = CCLabelBMFont::create("Hitbox Trail", "bigFont.fnt");
        trailLabel->setScale(0.4f);

        menu->addChild(noclipToggle);
        menu->addChild(hitboxToggle);
        menu->addChild(trailToggle);
        menu->updateLayout();

        this->addChild(menu);
        return true;
    }

    void onToggleNoclip(CCObject* sender) {
        bool val = !Mod::get()->getSettingValue<bool>("noclip");
        Mod::get()->setSettingValue<bool>("noclip", val);
    }

    void onToggleHitboxes(CCObject* sender) {
        bool val = !Mod::get()->getSettingValue<bool>("hitboxes");
        Mod::get()->setSettingValue<bool>("hitboxes", val);
        if (auto playLayer = PlayLayer::get()) {
            playLayer->toggleHitboxes();
        }
    }

    void onToggleTrail(CCObject* sender) {
        bool val = !Mod::get()->getSettingValue<bool>("hitbox-trail");
        Mod::get()->setSettingValue<bool>("hitbox-trail", val);
    }

public:
    static ModMenuPopup* create() {
        auto ret = new ModMenuPopup();
        if (ret && ret->initAnchored(320, 220)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

// Hook into PlayLayer to add the floating orb and enforce features
class $modify(MyPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        // Create the floating orb button on the game screen
        auto orbSprite = CCSprite::createWithSpriteFrameName("GJ_nodeBtn_001.png");
        auto orbButton = CCMenuItemSpriteExtra::create(
            orbSprite,
            this,
            menu_selector(MyPlayLayer::onOpenModMenu)
        );

        auto menu = CCMenu::create();
        menu->addChild(orbButton);
        menu->setPosition({40.f, CCDirector::get()->getWinSize().height - 40.f});
        menu->setID("mod-menu-floating-orb"_spr);
        this->addChild(menu, 100);

        return true;
    }

    void onOpenModMenu(CCObject*) {
        ModMenuPopup::create()->show();
    }

    // Noclip Hook: Prevent player death if noclip is enabled
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (Mod::get()->getSettingValue<bool>("noclip")) {
            return; // Ignore death!
        }
        PlayLayer::destroyPlayer(player, object);
    }
};
