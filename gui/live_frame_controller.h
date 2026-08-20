#ifndef PVT_LIVE_FRAME_CONTROLLER_H
#define PVT_LIVE_FRAME_CONTROLLER_H

#include "procedural_visualizer_tool.h"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

class LiveFrameController final : public QObject {
    Q_OBJECT

public:
    struct Result {
        QImage image;
        QString error;
        double render_milliseconds = 0.0;
        std::uint64_t sequence = 0U;
        std::uint64_t session_generation = 0U;
        std::uint64_t document_revision = 0U;
        std::uint64_t dropped_requests = 0U;
        bool late = false;
        bool cancelled = false;
        bool watchdog_expired = false;
    };

    explicit LiveFrameController(QObject* parent = nullptr);
    ~LiveFrameController() override;

    void request(pvt::ProjectConfig project, double normalizedPhase,
                 std::optional<int> synchronizedFrame,
                 const QSize& displayPixels, double resolutionScale,
                 double deadlineMilliseconds, int watchdogMilliseconds,
                 const pvt::FrameRenderOptions& options,
                 std::uint64_t sessionGeneration,
                 std::uint64_t documentRevision);
    void cancelCurrent();
    void stop();
    bool isRendering() const;
    std::uint64_t droppedRequests() const noexcept;

signals:
    void frameFinished(const LiveFrameController::Result& result);
    void watchdogExpired(std::uint64_t sequence,
                         std::uint64_t sessionGeneration,
                         std::uint64_t documentRevision);

private:
    struct Request {
        pvt::ProjectConfig project;
        double phase = 0.0;
        std::optional<int> synchronized_frame;
        QSize display_pixels;
        double resolution_scale = 1.0;
        double deadline_milliseconds = 100.0;
        int watchdog_milliseconds = 100;
        pvt::FrameRenderOptions options;
        std::uint64_t sequence = 0U;
        std::uint64_t session_generation = 0U;
        std::uint64_t document_revision = 0U;
        std::uint64_t dropped_requests = 0U;
    };

    static Result render(Request request,
                         const std::shared_ptr<std::atomic_bool>& cancel,
                         const std::shared_ptr<std::atomic_bool>& watchdogExpired);
    void launch(Request request);

    QFutureWatcher<Result> watcher_;
    std::optional<Request> pending_;
    std::shared_ptr<std::atomic_bool> cancel_;
    std::shared_ptr<std::atomic_bool> watchdog_expired_;
    QTimer watchdog_timer_;
    std::uint64_t next_sequence_ = 1U;
    std::uint64_t active_sequence_ = 0U;
    std::uint64_t active_session_generation_ = 0U;
    std::uint64_t active_document_revision_ = 0U;
    std::uint64_t dropped_requests_ = 0U;
    bool stopping_ = false;
};

Q_DECLARE_METATYPE(LiveFrameController::Result)

#endif
