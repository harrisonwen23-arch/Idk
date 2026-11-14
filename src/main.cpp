#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>

using namespace geode::prelude;

class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        auto winSize = CCDirector::get()->getWinSize();

        // 1. Add custom banner label at the top of the main menu
        auto bannerText = Mod::get()->getSettingValue<std::string>("banner-text");
        auto label = CCLabelBMFont::create(bannerText.c_str(), "bigFont.fnt");
        label->setPosition({winSize.width / 2, winSize.height - 22.f});
        label->setScale(0.4f);
        label->setID("custom-banner-label"_spr);
        this->addChild(label);

        // 2. Add an interactive button to the bottom menu bar
        auto btnSprite = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        auto myButton = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(MyMenuLayer::onMyButton)
        );
        myButton->setID("custom-info-button"_spr);

        if (auto menu = this->getChildByID("bottom-menu")) {
            menu->addChild(myButton);
            menu->updateLayout();
        }

        // 3. Show welcome popup once per game launch if enabled in settings
        static bool s_hasShownPopup = false;
        if (!s_hasShownPopup && Mod::get()->getSettingValue<bool>("show-popup")) {
            s_hasShownPopup = true;
            auto msg = Mod::get()->getSettingValue<std::string>("welcome-message");
            FLAlertLayer::create(
                "Custom Menu Mod",
                msg.c_str(),
                "Awesome!"
            )->show();
        }

        return true;
    }

    void onMyButton(CCObject*) {
        auto msg = Mod::get()->getSettingValue<std::string>("welcome-message");
        FLAlertLayer::create(
            "Mod Info",
            fmt::format("Current message:\n{}", msg),
            "Close"
        )->show();
    }
};
