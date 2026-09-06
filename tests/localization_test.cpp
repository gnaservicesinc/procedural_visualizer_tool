#include "localization.h"
#include "renderer_labels.h"

#include <QApplication>
#include <QLabel>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QString translated() {
    return QCoreApplication::translate("LocalizationTest", "Open %1");
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings_directory;
    require(settings_directory.isValid(), "Temporary settings directory unavailable");
    app.setOrganizationName(QStringLiteral("PVT Localization Tests"));
    app.setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_directory.path());
    require(Localization::savedLanguage() == QStringLiteral("system"), "Default should follow system");
    Localization::saveLanguage(QStringLiteral("de"));
    require(Localization::savedLanguage() == QStringLiteral("de"), "Language preference did not persist");

    const QLocale numeric_locale;
    Localization localization(QStringLiteral(":/test-i18n"));
    const auto available = localization.availableLanguages();
    require(available.contains(QStringLiteral("en")) && available.contains(QStringLiteral("de")),
            "Embedded catalogs missing");
    require(!available.contains(QStringLiteral("fr")), "Unfinished catalog advertised as supported");

    localization.install(app, QStringLiteral("system"), {QStringLiteral("fr"), QStringLiteral("de-CH")});
    require(localization.language() == QStringLiteral("de"), "Ordered system or regional fallback failed");
    require(translated().arg(QStringLiteral("test.pvt")) == QStringLiteral("[de] Open test.pvt"),
            "Catalog translation or placeholder substitution failed");
    require(localization.hasQtTranslation(), "Standard Qt translations were not embedded/loaded");
    require(QCoreApplication::translate("QPlatformTheme", "Cancel") != QStringLiteral("Cancel"),
            "Qt standard buttons still use English");
    require(renderer_label("Ripple") == QStringLiteral("[de] Ripple"), "Renderer GUI label context failed");
    require(QCoreApplication::translate("LocalizationTest", "%n frame(s)", nullptr, 1)
                == QStringLiteral("[de] 1 frame"), "Singular form failed");
    require(QCoreApplication::translate("LocalizationTest", "%n frame(s)", nullptr, 3)
                == QStringLiteral("[de] 3 frames"), "Plural form failed");
    require(QCoreApplication::translate("LocalizationTest", "Untranslated") == QStringLiteral("Untranslated"),
            "Missing message must fall back to source");

    localization.install(app, QStringLiteral("en"), {QStringLiteral("de")});
    require(translated() == QStringLiteral("Open %1") && !localization.hasQtTranslation(),
            "Explicit English must clear both translators");
    localization.install(app, QStringLiteral("system"), {QStringLiteral("en-GB"), QStringLiteral("de")});
    require(localization.language() == QStringLiteral("en"), "English preference was skipped");
    localization.install(app, QStringLiteral("zz-invalid"), {QStringLiteral("de")});
    require(localization.language() == QStringLiteral("en"), "Unknown override should fall back safely");

    localization.install(app, QStringLiteral("fa-IR"), {});
    QLabel rtl_label;
    require(app.layoutDirection() == Qt::RightToLeft && rtl_label.isRightToLeft(), "RTL layout failed");
    require(translated() == QStringLiteral("[fa] Open %1"), "RTL catalog failed to load");
    require(QLocale() == numeric_locale, "UI language changed numeric locale");

    localization.install(app, QStringLiteral("pt-BR"), {});
    require(localization.language() == QStringLiteral("pt_BR"), "Regional catalog was not selected");
    localization.install(app, QStringLiteral("zh-Hant-TW"), {});
    require(translated() == QStringLiteral("[zh_TW] Open %1"), "Traditional Chinese script fallback failed");
    localization.install(app, QStringLiteral("zh-Hans-CN"), {});
    require(translated() == QStringLiteral("[zh_CN] Open %1"), "Simplified Chinese script fallback failed");
    localization.install(app, QStringLiteral("sr-Latn-RS"), {});
    require(translated() == QStringLiteral("[sr_Latn] Open %1"), "Explicit script selection failed");
    localization.install(app, QStringLiteral("en"), {});
    require(app.layoutDirection() == Qt::LeftToRight, "LTR restoration failed");
    require(Localization::savedLanguage() == QStringLiteral("de"), "Runtime override changed saved preference");
    std::cout << "Localization resources, fallback, plurals, RTL and settings passed\n";
}
