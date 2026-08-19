#include "live_frame_controller.h"

#include <QtConcurrent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>

namespace {

float linear_to_srgb(float value) {
    if (!std::isfinite(value)) return 0.0F;
    const float encoded = value <= 0.0031308F
        ? 12.92F * value
        : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return std::clamp(encoded, 0.0F, 1.0F);
}

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
        emit watchdogExpired(active_sequence_);
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
                                  const QSize& display_pixels,
                                  double resolution_scale,
                                  int watchdog_milliseconds,
                                  const pvt::FrameRenderOptions& options) {
    if (!std::isfinite(normalized_phase)) return;
    Request request;
    request.project = std::move(project);
    request.phase = normalized_phase - std::floor(normalized_phase);
    request.display_pixels = display_pixels;
    request.resolution_scale = resolution_scale;
    request.watchdog_milliseconds = std::clamp(watchdog_milliseconds, 1, 60000);
    request.options = options;
    request.sequence = next_sequence_++;
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

void LiveFrameController::stop() {
    stopping_ = true;
    pending_.reset();
    watchdog_timer_.stop();
    if (cancel_ != nullptr) cancel_->store(true, std::memory_order_relaxed);
    if (watcher_.isRunning()) watcher_.waitForFinished();
    cancel_.reset();
    watchdog_expired_.reset();
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
        if (!pvt::render_project_frame_at_phase(
                request.project, request.phase, request.options, image,
                cancel != nullptr ? cancel.get() : nullptr, &error)) {
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
                for (int y = 0; y < image.height; ++y) {
                    if (cancel != nullptr
                        && cancel->load(std::memory_order_relaxed)) {
                        result.image = {};
                        result.cancelled = true;
                        break;
                    }
                    auto* row = result.image.scanLine(y);
                    for (int x = 0; x < image.width; ++x) {
                        const float* pixel = image.pixel(x, y);
                        row[x * 4] = static_cast<unsigned char>(std::lround(
                            linear_to_srgb(pixel[0]) * 255.0F));
                        row[x * 4 + 1] = static_cast<unsigned char>(std::lround(
                            linear_to_srgb(pixel[1]) * 255.0F));
                        row[x * 4 + 2] = static_cast<unsigned char>(std::lround(
                            linear_to_srgb(pixel[2]) * 255.0F));
                        row[x * 4 + 3] = static_cast<unsigned char>(std::lround(
                            std::clamp(pixel[3], 0.0F, 1.0F) * 255.0F));
                    }
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
                         > static_cast<double>(request.watchdog_milliseconds);
    return result;
}
