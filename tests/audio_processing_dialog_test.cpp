#include "audio_processing_dialog.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QPushButton>
#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; return 1; } } while (false)
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    pvt::AudioInputProcessingConfig initial;
    initial.music_onset_detection = pvt::MusicOnsetDetection::NeighborFlux;
    AudioProcessingDialog dialog(initial, QStringLiteral("Test music"), nullptr, true);
    dialog.show();
    app.processEvents();
    auto* selector = dialog.findChild<QComboBox*>(QStringLiteral("musicOnsetDetection"));
    auto* group = dialog.findChild<QGroupBox*>(QStringLiteral("musicDetectionGroup"));
    CHECK(selector && group && group->isVisible());
    CHECK(selector->count() == 4 && selector->currentData().toInt() == 2);
    CHECK(dialog.processing().music_onset_detection == initial.music_onset_detection);
    selector->setCurrentIndex(3);
    CHECK(dialog.processing().music_onset_detection == pvt::MusicOnsetDetection::HighFrequencyFlux);
    CHECK(initial.music_onset_detection == pvt::MusicOnsetDetection::NeighborFlux);
    if (argc > 1) CHECK(dialog.grab().save(QString::fromLocal8Bit(argv[1])));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    CHECK(buttons);
    buttons->button(QDialogButtonBox::Ok)->click();
    CHECK(dialog.result() == QDialog::Accepted);
    AudioProcessingDialog live(initial, QStringLiteral("Live input"));
    live.show();
    app.processEvents();
    CHECK(!live.findChild<QGroupBox*>(QStringLiteral("musicDetectionGroup"))->isVisible());
    CHECK(live.processing().music_onset_detection == initial.music_onset_detection);
    live.reject();
    CHECK(live.result() == QDialog::Rejected);
    std::cout << "Music detector selector, acceptance, and Live scope passed\n";
    return 0;
}
