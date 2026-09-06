#include "localization.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QSettings>

#include <utility>

Localization::Localization(QString resourceDirectory)
    : resource_directory_(std::move(resourceDirectory)) {}

QString Localization::savedLanguage() {
    return QSettings().value(QStringLiteral("preferences/language"),
                             QStringLiteral("system")).toString();
}

void Localization::saveLanguage(const QString& language) {
    QSettings settings;
    settings.setValue(QStringLiteral("preferences/language"), language);
    settings.sync();
}

QStringList Localization::availableLanguages() const {
    QStringList result{QStringLiteral("en")};
    const auto files = QDir(resource_directory_).entryList(
        {QStringLiteral("pvt_*.qm")}, QDir::Files, QDir::Name);
    for (const auto& file : files) {
        const QString code = file.mid(4, file.size() - 7);
        if (code == QStringLiteral("en")) continue;
        QTranslator probe;
        // Empty, unfinished catalogs must not advertise a translated UI.
        if (QLocale(code).language() != QLocale::C
            && probe.load(resource_directory_ + QLatin1Char('/') + file)
            && !probe.isEmpty()) {
            result.append(code);
        }
    }
    return result;
}

bool Localization::resourcesAvailable() const {
    QTranslator probe;
    return QFile::exists(resource_directory_ + QStringLiteral("/pvt_en.qm"))
        && probe.load(QStringLiteral(":/i18n/qt/qtbase_de.qm"))
        && !probe.isEmpty();
}

void Localization::install(QApplication& application,
                           const QString& requestedLanguage,
                           const QStringList& systemLanguages) {
    application.removeTranslator(&application_translator_);
    application.removeTranslator(&qt_translator_);
    language_ = QStringLiteral("en");
    qt_loaded_ = false;
    const QStringList preferences =
        requestedLanguage.isEmpty() || requestedLanguage == QStringLiteral("system")
            ? systemLanguages : QStringList{requestedLanguage};
    for (const auto& preference : preferences) {
        const QLocale locale(preference);
        if (locale.language() == QLocale::C) continue;
        // An explicit English preference terminates the search, including an
        // English entry ahead of another supported language in the OS list.
        if (locale.language() == QLocale::English) break;
        if (application_translator_.load(locale, QStringLiteral("pvt"),
                                          QStringLiteral("_"), resource_directory_)
            && !application_translator_.isEmpty()) {
            language_ = application_translator_.language();
            break;
        }
    }
    if (language_ == QStringLiteral("en")) {
        // Also permits an English catalog to supply English plural forms.
        (void)application_translator_.load(QStringLiteral("pvt_en"), resource_directory_);
    } else {
        qt_loaded_ = qt_translator_.load(
            QLocale(language_), QStringLiteral("qtbase"), QStringLiteral("_"),
            QStringLiteral(":/i18n/qt"));
        if (qt_loaded_) application.installTranslator(&qt_translator_);
    }
    if (!application_translator_.isEmpty()) {
        application.installTranslator(&application_translator_);
    }
    application.setLayoutDirection(QLocale(language_).textDirection());
    // Keep the user's numeric/date locale. Never set the C/C++ process locale:
    // project codecs, OSC addresses and renderer identifiers are locale-neutral.
}
