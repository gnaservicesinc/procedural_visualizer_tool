#ifndef PVT_LOCALIZATION_H
#define PVT_LOCALIZATION_H

#include <QStringList>
#include <QTranslator>

class QApplication;

// Construct before any widgets and keep alive for the application's lifetime.
// Language changes take effect on restart; authored project data is untouched.
class Localization final {
public:
    explicit Localization(QString resourceDirectory = QStringLiteral(":/i18n"));
    static QString savedLanguage();
    static void saveLanguage(const QString& language);
    QStringList availableLanguages() const;
    bool resourcesAvailable() const;
    void install(QApplication& application, const QString& requestedLanguage,
                 const QStringList& systemLanguages);
    QString language() const { return language_; }
    bool hasQtTranslation() const { return qt_loaded_; }

private:
    QString resource_directory_;
    QString language_ = QStringLiteral("en");
    QTranslator application_translator_;
    QTranslator qt_translator_;
    bool qt_loaded_ = false;
};

#endif
