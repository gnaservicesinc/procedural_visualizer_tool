#include "live_frame_controller.h"
#include "display_color.h"

#include <QtConcurrent>

#include <QColorSpace>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

namespace {

void scale_project_for_stage(pvt::ProjectConfig& project,
                             const QSize& display_pixels,
                             double requested_scale) {
    const int source_width = project.canvas.width;
    const int source_height = project.canvas.height;
    const int source_block_size = project.canvas.block_size;
    if (source_width <= 0 || source_height <= 0 || source_block_size <= 0) {
        return;
    }
    const int source_short_edge = std::max(
        1, std::min(source_width, source_height));
    requested_scale = std::clamp(requested_scale, 0.10, 1.0);
    const int target_width = std::max(16, display_pixels.width());
    const int target_height = std::max(16, display_pixels.height());
    const double fit = std::min(
        {1.0,
         static_cast<double>(target_width) / source_width,
         static_cast<double>(target_height) / source_height});
    const double scale = std::min(1.0, fit * requested_scale);
    const int width = std::max(
        16, static_cast<int>(std::lround(source_width * scale)));
    const int height = std::max(
        16, static_cast<int>(std::lround(source_height * scale)));
    const double pixel_scale = static_cast<double>(std::min(width, height))
                               / source_short_edge;
    for (auto& layer : project.layers) {
        layer.render.starting_colors.reference_width = source_width;
        layer.render.starting_colors.reference_height = source_height;
        layer.render.starting_colors.reference_block_size = source_block_size;
        layer.render.displacement *= pixel_scale;
        for (auto& effect : layer.render.effects) {
            if (effect.type == pvt::EffectType::Glow
                || effect.type == pvt::EffectType::ParticleField
                || effect.type == pvt::EffectType::Blur) {
                effect.radius_pixels *= pixel_scale;
            } else if (effect.type == pvt::EffectType::EdgeDetect) {
                effect.frequency = std::max(
                    1.0, std::round(effect.frequency * pixel_scale));
            }
        }
    }
    project.canvas.width = width;
    project.canvas.height = height;
    project.canvas.block_size = std::max(
        1, static_cast<int>(std::lround(source_block_size * scale)));
}

} // namespace

LiveFrameController::LiveFrameController(QObject* parent) : QObject(parent) {
    qRegisterMetaType<LiveFrameController::Result>();
    watchdog_timer_.setSingleShot(true);
    connect(&watchdog_timer_, &QTimer::timeout, this, [this] {
        if (!watcher_.isRunning() || cancel_ == nullptr
            || watchdog_expired_ == nullptr) {
            return;
        }
        watchdog_expired_->store(true, std::memory_order_relaxed);
        cancel_->store(true, std::memory_order_relaxed);
        emit watchdogExpired(active_sequence_, active_session_generation_,
                             active_document_revision_);
    });
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
        watchdog_timer_.stop();
        Result result;
        try {
            result = watcher_.result();
        } catch (const std::exception& exception) {
            result.error = tr("Live renderer failed: %1")
                               .arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            result.error = tr("Live renderer failed unexpectedly.");
        }
        if (!stopping_) emit frameFinished(result);
        if (!stopping_ && pending_) {
            Request next = std::move(*pending_);
            pending_.reset();
            launch(std::move(next));
        }
    });
}

LiveFrameController::~LiveFrameController() {
    stop();
}

void LiveFrameController::request(pvt::ProjectConfig project,
                                  double normalized_phase,
                                  std::optional<int> synchronized_frame,
                                  const QSize& display_pixels,
                                  double resolution_scale,
                                  double deadline_milliseconds,
                                  int watchdog_milliseconds,
                                  const pvt::FrameRenderOptions& options,
                                  std::uint64_t session_generation,
                                  std::uint64_t document_revision) {
    if (!std::isfinite(normalized_phase)) return;
    Request request;
    request.project = std::move(project);
    request.phase = normalized_phase - std::floor(normalized_phase);
    request.synchronized_frame = synchronized_frame;
    request.display_pixels = display_pixels;
    request.resolution_scale = resolution_scale;
    request.deadline_milliseconds = std::isfinite(deadline_milliseconds)
        ? std::max(0.001, deadline_milliseconds) : 100.0;
    request.watchdog_milliseconds = std::max(1, watchdog_milliseconds);
    request.options = options;
    request.sequence = next_sequence_++;
    request.session_generation = session_generation;
    request.document_revision = document_revision;
    request.dropped_requests = dropped_requests_;
    if (watcher_.isRunning()) {
        if (pending_) ++dropped_requests_;
        request.dropped_requests = dropped_requests_;
        pending_ = std::move(request);
        return;
    }
    launch(std::move(request));
}

void LiveFrameController::launch(Request request) {
    stopping_ = false;
    active_sequence_ = request.sequence;
    active_session_generation_ = request.session_generation;
    active_document_revision_ = request.document_revision;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    watchdog_expired_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = cancel_;
    const auto watchdog_expired = watchdog_expired_;
    watchdog_timer_.start(request.watchdog_milliseconds);
    watcher_.setFuture(QtConcurrent::run(
        [request = std::move(request), cancel, watchdog_expired]() mutable {
            return render(std::move(request), cancel, watchdog_expired);
        }));
}

void LiveFrameController::cancelCurrent() {
    pending_.reset();
    if (cancel_ != nullptr) cancel_->store(true, std::memory_order_relaxed);
}

void LiveFrameController::stop() {
    stopping_ = true;
    pending_.reset();
    watchdog_timer_.stop();
    if (cancel_ != nullptr) cancel_->store(true, std::memory_order_relaxed);
    if (watcher_.isRunning()) watcher_.waitForFinished();
    cancel_.reset();
    watchdog_expired_.reset();
    dropped_requests_ = 0U;
    stopping_ = false;
}

bool LiveFrameController::isRendering() const { return watcher_.isRunning(); }
std::uint64_t LiveFrameController::droppedRequests() const noexcept {
    return dropped_requests_;
}

LiveFrameController::Result LiveFrameController::render(
    Request request, const std::shared_ptr<std::atomic_bool>& cancel,
    const std::shared_ptr<std::atomic_bool>& watchdog_expired) {
    Result result;
    result.sequence = request.sequence;
    result.session_generation = request.session_generation;
    result.document_revision = request.document_revision;
    result.dropped_requests = request.dropped_requests;
    const auto started = std::chrono::steady_clock::now();
    try {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.watchdog_expired = watchdog_expired != nullptr
                && watchdog_expired->load(std::memory_order_relaxed);
            result.late = result.watchdog_expired;
            return result;
        }
        scale_project_for_stage(request.project, request.display_pixels,
                                request.resolution_scale);
        pvt::Image image;
        std::string error;
        const bool rendered = request.synchronized_frame.has_value()
            ? pvt::render_project_frame(
                  request.project, *request.synchronized_frame,
                  request.options, image,
                  cancel != nullptr ? cancel.get() : nullptr, &error)
            : pvt::render_project_frame_at_phase(
                  request.project, request.phase, request.options, image,
                  cancel != nullptr ? cancel.get() : nullptr, &error);
        if (!rendered) {
            result.cancelled = cancel != nullptr
                && cancel->load(std::memory_order_relaxed);
            if (!result.cancelled) result.error = QString::fromStdString(error);
        } else {
            result.image = QImage(image.width, image.height,
                                  QImage::Format_RGBA8888);
            if (result.image.isNull()) {
                result.error = QObject::tr(
                    "The live frame display buffer could not be allocated.");
            } else {
                result.image.setColorSpace(QColorSpace::SRgb);
                for (int y = 0; y < image.height; ++y) {
                    if (cancel != nullptr
                        && cancel->load(std::memory_order_relaxed)) {
                        result.image = {};
                        result.cancelled = true;
                        break;
                    }
                    auto* row = result.image.scanLine(y);
                    pvt::display::convert_rgba_row(
                        image.pixel(0, y), row,
                        static_cast<std::size_t>(image.width));
                }
            }
        }
    } catch (const std::exception& exception) {
        result.image = {};
        result.error = QObject::tr("Live render failed: %1")
                           .arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        result.image = {};
        result.error = QObject::tr("Live render failed unexpectedly.");
    }
    result.render_milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    result.watchdog_expired = watchdog_expired != nullptr
        && watchdog_expired->load(std::memory_order_relaxed);
    result.late = result.watchdog_expired
                  || result.render_milliseconds
                         > request.deadline_milliseconds;
    return result;
}
