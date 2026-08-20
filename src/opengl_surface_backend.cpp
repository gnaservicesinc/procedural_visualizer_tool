#include "frame_renderer_internal.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QScreen>
#include <QSurfaceFormat>
#include <QThread>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

thread_local bool g_surface_acceleration_active = false;

constexpr const char* kVertexShader = R"PVT_GLSL(#version 330 core
out vec2 unusedUv;
void main() {
    vec2 position = vec2((gl_VertexID == 1) ? 3.0 : -1.0,
                         (gl_VertexID == 2) ? 3.0 : -1.0);
    unusedUv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)PVT_GLSL";

constexpr const char* kFragmentShader = R"PVT_GLSL(#version 330 core
uniform sampler2D sourceImage;
uniform ivec2 imageSize;
uniform int mapping;
uniform int projection;
uniform int sizing;
uniform int outsideMode;
uniform int compositeBackfaces;
uniform int rotationOrder;
uniform vec3 rotation;
uniform vec3 objectScale;
uniform vec3 position;
uniform float sizeMultiplier;
uniform float cameraDistance;
uniform float focalLength;
uniform float curvature;
uniform float lighting;
uniform vec3 lightDirection;
uniform float lightAmbient;
uniform float lightDiffuse;
out vec4 outputColor;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;

struct Hit {
    float distance;
    vec3 point;
    vec3 normal;
};

float clamp01(float value) {
    return clamp(value, 0.0, 1.0);
}

float wrapUnit(float value) {
    float wrapped = mod(value, 1.0);
    return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
}

int reflectedIndex(int index, int size) {
    if (size <= 1) return 0;
    int period = 2 * (size - 1);
    int value = index % period;
    if (value < 0) value += period;
    if (value >= size) value = period - value;
    return value;
}

vec4 loadPixel(int x, int y) {
    return texelFetch(sourceImage, ivec2(x, y), 0);
}

vec4 sampleReflect(vec2 coordinate) {
    ivec2 first = ivec2(floor(coordinate));
    vec2 amount = coordinate - vec2(first);
    vec4 top = mix(
        loadPixel(reflectedIndex(first.x, imageSize.x),
                  reflectedIndex(first.y, imageSize.y)),
        loadPixel(reflectedIndex(first.x + 1, imageSize.x),
                  reflectedIndex(first.y, imageSize.y)), amount.x);
    vec4 bottom = mix(
        loadPixel(reflectedIndex(first.x, imageSize.x),
                  reflectedIndex(first.y + 1, imageSize.y)),
        loadPixel(reflectedIndex(first.x + 1, imageSize.x),
                  reflectedIndex(first.y + 1, imageSize.y)), amount.x);
    vec4 result = mix(top, bottom, amount.y);
    result.a = clamp01(result.a);
    return result;
}

vec4 sampleWrappedX(vec2 coordinate) {
    float width = float(imageSize.x);
    float wrappedX = mod(coordinate.x, width);
    if (wrappedX < 0.0) wrappedX += width;
    int x0 = int(floor(wrappedX));
    int x1 = (x0 + 1) % imageSize.x;
    int y0 = int(floor(coordinate.y));
    vec2 amount = vec2(wrappedX - float(x0), coordinate.y - float(y0));
    vec4 top = mix(
        loadPixel(x0, reflectedIndex(y0, imageSize.y)),
        loadPixel(x1, reflectedIndex(y0, imageSize.y)), amount.x);
    vec4 bottom = mix(
        loadPixel(x0, reflectedIndex(y0 + 1, imageSize.y)),
        loadPixel(x1, reflectedIndex(y0 + 1, imageSize.y)), amount.x);
    vec4 result = mix(top, bottom, amount.y);
    result.a = clamp01(result.a);
    return result;
}

vec3 rotateX(vec3 value, float angle) {
    float cosine = cos(angle);
    float sine = sin(angle);
    return vec3(value.x, cosine * value.y - sine * value.z,
                sine * value.y + cosine * value.z);
}

vec3 rotateY(vec3 value, float angle) {
    float cosine = cos(angle);
    float sine = sin(angle);
    return vec3(cosine * value.x + sine * value.z, value.y,
                -sine * value.x + cosine * value.z);
}

vec3 rotateZ(vec3 value, float angle) {
    float cosine = cos(angle);
    float sine = sin(angle);
    return vec3(cosine * value.x - sine * value.y,
                sine * value.x + cosine * value.y, value.z);
}

vec3 rotateSurface(vec3 value) {
    if (rotationOrder == 0) return rotateZ(rotateY(rotateX(value, rotation.x), rotation.y), rotation.z);
    if (rotationOrder == 1) return rotateY(rotateZ(rotateX(value, rotation.x), rotation.z), rotation.y);
    if (rotationOrder == 2) return rotateZ(rotateX(rotateY(value, rotation.y), rotation.x), rotation.z);
    if (rotationOrder == 3) return rotateX(rotateZ(rotateY(value, rotation.y), rotation.z), rotation.x);
    if (rotationOrder == 4) return rotateY(rotateX(rotateZ(value, rotation.z), rotation.x), rotation.y);
    return rotateX(rotateY(rotateZ(value, rotation.z), rotation.y), rotation.x);
}

vec3 inverseRotateSurface(vec3 value) {
    if (rotationOrder == 0) return rotateX(rotateY(rotateZ(value, -rotation.z), -rotation.y), -rotation.x);
    if (rotationOrder == 1) return rotateX(rotateZ(rotateY(value, -rotation.y), -rotation.z), -rotation.x);
    if (rotationOrder == 2) return rotateY(rotateX(rotateZ(value, -rotation.z), -rotation.x), -rotation.y);
    if (rotationOrder == 3) return rotateY(rotateZ(rotateX(value, -rotation.x), -rotation.z), -rotation.y);
    if (rotationOrder == 4) return rotateZ(rotateX(rotateY(value, -rotation.y), -rotation.x), -rotation.z);
    return rotateZ(rotateY(rotateX(value, -rotation.x), -rotation.y), -rotation.z);
}

vec3 objectRay(vec3 value) {
    return inverseRotateSurface(value) / objectScale;
}

vec3 worldNormal(vec3 normal) {
    return normalize(rotateSurface(normal / objectScale));
}

vec3 faceForwardToRay(vec3 normal, vec3 rayDirection) {
    return dot(normal, rayDirection) > 0.0 ? -normal : normal;
}

vec4 shadeSurface(vec4 color, vec3 normal, float amount) {
    vec3 light = normalize(lightDirection);
    float diffuse = max(0.0, dot(normalize(normal), light));
    float lit = lightAmbient + lightDiffuse * diffuse;
    float multiplier = max(0.0, 1.0 + amount * (lit - 1.0));
    color.rgb *= multiplier;
    return color;
}

vec4 blendStraight(vec4 first, vec4 second, float amount) {
    return mix(first, second, clamp01(amount));
}

vec4 compositeStraightOver(vec4 front, vec4 back) {
    float frontAlpha = clamp01(front.a);
    float backWeight = clamp01(back.a) * (1.0 - frontAlpha);
    float resultAlpha = frontAlpha + backWeight;
    if (resultAlpha <= 1.0e-12) return vec4(front.rgb, 0.0);
    return vec4((front.rgb * frontAlpha + back.rgb * backWeight)
                    / resultAlpha,
                resultAlpha);
}

vec3 cubeNormal(vec3 point) {
    vec3 absolute = abs(point);
    if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
        return vec3(point.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    }
    if (absolute.y >= absolute.x && absolute.y >= absolute.z) {
        return vec3(0.0, point.y >= 0.0 ? 1.0 : -1.0, 0.0);
    }
    return vec3(0.0, 0.0, point.z >= 0.0 ? 1.0 : -1.0);
}

bool intersectCube(vec3 origin, vec3 direction,
                   out Hit front, out Hit back, out bool hasBack) {
    float nearDistance = -3.402823466e38;
    float farDistance = 3.402823466e38;
    for (int axis = 0; axis < 3; ++axis) {
        float axisOrigin = origin[axis];
        float axisDirection = direction[axis];
        if (abs(axisDirection) < 1.0e-12) {
            if (axisOrigin < -1.0 || axisOrigin > 1.0) return false;
            continue;
        }
        float first = (-1.0 - axisOrigin) / axisDirection;
        float second = (1.0 - axisOrigin) / axisDirection;
        if (first > second) {
            float temporary = first;
            first = second;
            second = temporary;
        }
        nearDistance = max(nearDistance, first);
        farDistance = min(farDistance, second);
        if (nearDistance > farDistance) return false;
    }
    if (farDistance < 0.0) return false;
    front.distance = nearDistance >= 0.0 ? nearDistance : farDistance;
    front.point = origin + direction * front.distance;
    front.normal = cubeNormal(front.point);
    hasBack = nearDistance >= 0.0
              && farDistance - nearDistance > 1.0e-10;
    if (hasBack) {
        back.distance = farDistance;
        back.point = origin + direction * farDistance;
        back.normal = cubeNormal(back.point);
    }
    return true;
}

vec2 cubeUv(vec3 point, vec3 normal) {
    float u = 0.5;
    float v = 0.5;
    if (abs(normal.x) > 0.5) {
        u = normal.x > 0.0 ? (1.0 - point.z) * 0.5
                           : (point.z + 1.0) * 0.5;
        v = (1.0 - point.y) * 0.5;
    } else if (abs(normal.y) > 0.5) {
        u = (point.x + 1.0) * 0.5;
        v = normal.y > 0.0 ? (point.z + 1.0) * 0.5
                           : (1.0 - point.z) * 0.5;
    } else {
        u = normal.z > 0.0 ? (point.x + 1.0) * 0.5
                           : (1.0 - point.x) * 0.5;
        v = (1.0 - point.y) * 0.5;
    }
    return clamp(vec2(u, v), vec2(0.0), vec2(1.0));
}

void addCylinderHit(inout Hit hits[4], inout int count, float distance,
                    vec3 origin, vec3 direction, vec3 normal) {
    if (isnan(distance) || isinf(distance) || distance < 0.0 || count >= 4) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        if (abs(hits[index].distance - distance) <= 1.0e-7) return;
    }
    hits[count].distance = distance;
    hits[count].point = origin + direction * distance;
    hits[count].normal = normal;
    ++count;
}

bool intersectCylinder(vec3 origin, vec3 direction,
                       out Hit front, out Hit back, out bool hasBack) {
    Hit hits[4];
    int count = 0;
    float a = direction.x * direction.x + direction.z * direction.z;
    float b = 2.0 * (origin.x * direction.x
                     + origin.z * direction.z);
    float c = origin.x * origin.x + origin.z * origin.z - 1.0;
    if (a > 1.0e-12) {
        float discriminant = b * b - 4.0 * a * c;
        if (discriminant >= 0.0) {
            float root = sqrt(max(0.0, discriminant));
            float candidates[2] = float[2]((-b - root) / (2.0 * a),
                                            (-b + root) / (2.0 * a));
            for (int index = 0; index < 2; ++index) {
                float distance = candidates[index];
                float y = origin.y + distance * direction.y;
                if (distance >= 0.0 && y >= -1.0000001 && y <= 1.0000001) {
                    vec3 point = origin + direction * distance;
                    addCylinderHit(hits, count, distance, origin, direction,
                                   normalize(vec3(point.x, 0.0, point.z)));
                }
            }
        }
    }
    if (abs(direction.y) > 1.0e-12) {
        for (int index = 0; index < 2; ++index) {
            float capY = index == 0 ? -1.0 : 1.0;
            float distance = (capY - origin.y) / direction.y;
            vec3 point = origin + direction * distance;
            if (distance >= 0.0
                && point.x * point.x + point.z * point.z <= 1.0000001) {
                addCylinderHit(hits, count, distance, origin, direction,
                               vec3(0.0, capY > 0.0 ? 1.0 : -1.0, 0.0));
            }
        }
    }
    if (count == 0) return false;
    for (int firstIndex = 0; firstIndex < 4; ++firstIndex) {
        for (int secondIndex = firstIndex + 1; secondIndex < 4; ++secondIndex) {
            if (secondIndex < count
                && hits[secondIndex].distance < hits[firstIndex].distance) {
                Hit temporary = hits[firstIndex];
                hits[firstIndex] = hits[secondIndex];
                hits[secondIndex] = temporary;
            }
        }
    }
    front = hits[0];
    hasBack = count > 1;
    if (hasBack) back = hits[1];
    return true;
}

vec2 cylinderUv(vec3 point, vec3 normal) {
    if (abs(normal.y) < 0.5) {
        return vec2(wrapUnit(0.5 + atan(point.x, point.z) / TAU),
                    clamp01(0.5 * (1.0 - point.y)));
    }
    float u = 0.5 + 0.5 * point.x;
    float v = normal.y > 0.0 ? 0.5 + 0.5 * point.z
                             : 0.5 - 0.5 * point.z;
    return clamp(vec2(u, v), vec2(0.0), vec2(1.0));
}

vec4 shadeHit(vec4 sampled, vec3 normal, vec3 worldDirection) {
    return shadeSurface(sampled,
                        faceForwardToRay(worldNormal(normal), worldDirection),
                        lighting * curvature);
}

vec4 sampleCylinderHit(Hit hit, vec3 worldDirection) {
    vec2 uv = cylinderUv(hit.point, hit.normal);
    vec4 sampled = abs(hit.normal.y) < 0.5
        ? sampleWrappedX(vec2(uv.x * float(imageSize.x),
                              uv.y * float(imageSize.y - 1)))
        : sampleReflect(vec2(uv.x * float(imageSize.x - 1),
                             uv.y * float(imageSize.y - 1)));
    return shadeHit(sampled, hit.normal, worldDirection);
}

bool intersectSphere(vec3 origin, vec3 direction,
                     out Hit front, out Hit back, out bool hasBack) {
    float a = dot(direction, direction);
    float b = 2.0 * dot(origin, direction);
    float c = dot(origin, origin) - 1.0;
    float discriminant = b * b - 4.0 * a * c;
    if (a <= 1.0e-12 || discriminant < 0.0) return false;
    float root = sqrt(max(0.0, discriminant));
    float first = (-b - root) / (2.0 * a);
    float second = (-b + root) / (2.0 * a);
    if (first > second) {
        float temporary = first;
        first = second;
        second = temporary;
    }
    if (second < 0.0) return false;
    front.distance = first >= 0.0 ? first : second;
    front.point = origin + direction * front.distance;
    front.normal = normalize(front.point);
    hasBack = first >= 0.0 && second - first > 1.0e-10;
    if (hasBack) {
        back.distance = second;
        back.point = origin + direction * second;
        back.normal = normalize(back.point);
    }
    return true;
}

vec4 sampleSphereHit(Hit hit, vec3 worldDirection) {
    float longitude = atan(hit.normal.x, hit.normal.z);
    float latitude = asin(clamp(hit.normal.y, -1.0, 1.0));
    float u = wrapUnit(0.5 + longitude / TAU);
    float v = 0.5 - latitude / PI;
    vec4 sampled = sampleWrappedX(
        vec2(u * float(imageSize.x), v * float(imageSize.y - 1)));
    return shadeHit(sampled, hit.normal, worldDirection);
}

vec4 sampleCubeHit(Hit hit, vec3 worldDirection) {
    vec2 uv = cubeUv(hit.point, hit.normal);
    vec4 sampled = sampleReflect(
        uv * vec2(float(imageSize.x - 1), float(imageSize.y - 1)));
    return shadeHit(sampled, hit.normal, worldDirection);
}

void main() {
    int x = int(gl_FragCoord.x);
    int y = imageSize.y - 1 - int(gl_FragCoord.y);
    vec4 planar = loadPixel(x, y);
    float widthSpan = float(max(1, imageSize.x - 1));
    float heightSpan = float(max(1, imageSize.y - 1));
    float shortSide = float(min(imageSize.x, imageSize.y));
    float halfX = mapping == 0 ? widthSpan / heightSpan : 1.0;
    float containScale = min(widthSpan / (2.0 * halfX),
                             heightSpan * 0.5);
    float coverScale = max(widthSpan / (2.0 * halfX),
                           heightSpan * 0.5);
    float screenScaleX = containScale * sizeMultiplier;
    float screenScaleY = containScale * sizeMultiplier;
    if (sizing == 1) {
        screenScaleX = coverScale * sizeMultiplier;
        screenScaleY = coverScale * sizeMultiplier;
    } else if (sizing == 2) {
        screenScaleX = widthSpan / (2.0 * halfX) * sizeMultiplier;
        screenScaleY = heightSpan * 0.5 * sizeMultiplier;
    } else if (sizing == 3) {
        screenScaleX = 0.5 * shortSide * sizeMultiplier;
        screenScaleY = screenScaleX;
    }
    float centerX = 0.5 * widthSpan + position.x * widthSpan / 100.0;
    float centerY = 0.5 * heightSpan - position.y * heightSpan / 100.0;
    float screenX = (float(x) - centerX) / screenScaleX;
    float screenY = (centerY - float(y)) / screenScaleY;
    vec3 worldOrigin;
    vec3 worldDirection;
    if (projection == 1) {
        worldOrigin = vec3(0.0, 0.0, cameraDistance);
        worldDirection = normalize(vec3(screenX, screenY, -focalLength));
    } else {
        worldOrigin = vec3(screenX, screenY, cameraDistance);
        worldDirection = vec3(0.0, 0.0, -1.0);
    }
    vec3 origin = objectRay(worldOrigin - vec3(0.0, 0.0, position.z));
    vec3 direction = objectRay(worldDirection);
    vec4 mapped = vec4(0.0);
    bool visible = true;
    Hit front;
    Hit back;
    bool hasBack = false;

    if (mapping == 0) {
        if (abs(direction.z) <= 1.0e-12) {
            visible = false;
        } else {
            float distance = -origin.z / direction.z;
            if (distance < 0.0) {
                visible = false;
            } else {
                vec3 point = origin + direction * distance;
                bool inside = abs(point.x) <= halfX && abs(point.y) <= 1.0;
                if (!inside && outsideMode != 2) {
                    visible = false;
                } else {
                    float u = 0.5 + 0.5 * point.x / halfX;
                    float v = 0.5 - 0.5 * point.y;
                    mapped = shadeHit(sampleReflect(
                        vec2(u * widthSpan, v * heightSpan)),
                        vec3(0.0, 0.0, 1.0), worldDirection);
                }
            }
        }
    } else if (mapping == 1) {
        if (!intersectCylinder(origin, direction, front, back, hasBack)) {
            visible = false;
        } else {
            mapped = sampleCylinderHit(front, worldDirection);
            if (compositeBackfaces != 0 && hasBack) {
                mapped = compositeStraightOver(
                    mapped, sampleCylinderHit(back, worldDirection));
            }
        }
    } else if (mapping == 2) {
        if (!intersectSphere(origin, direction, front, back, hasBack)) {
            visible = false;
        } else {
            mapped = sampleSphereHit(front, worldDirection);
            if (compositeBackfaces != 0 && hasBack) {
                mapped = compositeStraightOver(
                    mapped, sampleSphereHit(back, worldDirection));
            }
        }
    } else {
        if (!intersectCube(origin, direction, front, back, hasBack)) {
            visible = false;
        } else {
            mapped = sampleCubeHit(front, worldDirection);
            if (compositeBackfaces != 0 && hasBack) {
                mapped = compositeStraightOver(
                    mapped, sampleCubeHit(back, worldDirection));
            }
        }
    }

    if (visible) {
        outputColor = mapping == 0 ? mapped
                                   : blendStraight(planar, mapped, curvature);
    } else if (outsideMode == 1 || (outsideMode == 2 && mapping != 0)) {
        outputColor = planar;
    } else {
        outputColor = blendStraight(planar, vec4(0.0), curvature);
    }
    outputColor.a = clamp01(outputColor.a);
}
)PVT_GLSL";

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

std::string gl_text(const GLubyte* text) {
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

class OpenGLSurfaceService final {
public:
    bool available(std::string* device_name, std::string* status) {
        std::lock_guard<std::mutex> guard(mutex_);
        ensure_initialized_locked();
        if (device_name != nullptr) *device_name = device_name_;
        if (status != nullptr) *status = status_;
        return ready_;
    }

    bool render(const Image& source, Image& destination,
                const SurfaceConfig& surface, double loop_phase,
                const std::atomic_bool* cancel, std::string* error) {
        if (cancelled(cancel)) {
            return fail(error,
                        "OpenGL surface rendering was cancelled; destination "
                        "was unchanged.");
        }
        std::lock_guard<std::mutex> guard(mutex_);
        ensure_initialized_locked();
        if (!ready_) return fail(error, status_);

        bool rendered = false;
        std::string render_error;
        const auto work = [&] {
            rendered = render_on_gpu_thread(source, destination, surface,
                                            loop_phase, &render_error);
        };
        if (QThread::currentThread() == render_thread_) {
            work();
        } else if (!QMetaObject::invokeMethod(
                       worker_, work, Qt::BlockingQueuedConnection)) {
            return fail(error,
                        "OpenGL could not dispatch work to its bounded render "
                        "thread.");
        }
        if (!rendered) return fail(error, std::move(render_error));
        if (cancelled(cancel)) {
            return fail(error,
                        "OpenGL surface rendering was cancelled; destination "
                        "was unchanged.");
        }
        if (error != nullptr) error->clear();
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> guard(mutex_);
        ready_ = false;

        if (render_thread_ != nullptr) {
            QThread* const gui_thread = QCoreApplication::instance() != nullptr
                ? QCoreApplication::instance()->thread()
                : QThread::currentThread();
            const auto release_gpu_objects = [this, gui_thread] {
                if (context_ != nullptr) {
                    if (context_->makeCurrent(surface_)) {
                        QOpenGLExtraFunctions* gl = context_->extraFunctions();
                        gl->initializeOpenGLFunctions();
                        if (vertex_array_ != 0U) {
                            gl->glDeleteVertexArrays(1, &vertex_array_);
                            vertex_array_ = 0U;
                        }
                        if (program_ != 0U) {
                            gl->glDeleteProgram(program_);
                            program_ = 0U;
                        }
                        context_->doneCurrent();
                    }
                    context_->moveToThread(gui_thread);
                }
                if (worker_ != nullptr) worker_->moveToThread(gui_thread);
            };
            if (QThread::currentThread() == render_thread_) {
                release_gpu_objects();
            } else {
                (void)QMetaObject::invokeMethod(
                    worker_, release_gpu_objects, Qt::BlockingQueuedConnection);
            }
            if (owns_render_thread_) {
                render_thread_->quit();
                render_thread_->wait();
            }
        }

        delete context_;
        context_ = nullptr;
        delete worker_;
        worker_ = nullptr;
        if (owns_render_thread_) delete render_thread_;
        render_thread_ = nullptr;
        owns_render_thread_ = false;
        delete surface_;
        surface_ = nullptr;
    }

private:
    void ensure_initialized_locked() {
        if (ready_ || initialization_attempted_) return;
        auto* application = qobject_cast<QGuiApplication*>(
            QCoreApplication::instance());
        if (application == nullptr) {
            status_ = "OpenGL surface acceleration is compiled, but a "
                      "QGuiApplication is required to create its offscreen "
                      "context.";
            return;
        }
        initialization_attempted_ = true;
        const auto initialize = [this] { initialize_on_gui_thread(); };
        if (QThread::currentThread() == application->thread()) {
            initialize();
        } else if (!QMetaObject::invokeMethod(
                       application, initialize, Qt::BlockingQueuedConnection)) {
            status_ = "OpenGL could not initialize on the Qt GUI thread.";
        }
    }

    void initialize_on_gui_thread() {
        struct ContextAttempt {
            QSurfaceFormat::OpenGLContextProfile profile;
            const char* name;
        };
        constexpr std::array<ContextAttempt, 3U> attempts {{
            {QSurfaceFormat::CoreProfile, "core profile"},
            {QSurfaceFormat::CompatibilityProfile, "compatibility profile"},
            {QSurfaceFormat::NoProfile, "default profile"},
        }};

        std::string attempt_status;
        std::string selected_attempt;
        std::string vendor;
        std::string renderer;
        std::string version;
        const auto discard_candidate = [this] {
            delete surface_;
            surface_ = nullptr;
            delete context_;
            context_ = nullptr;
        };
        const auto record_failure = [&attempt_status](
                                        const char* name,
                                        const char* reason) {
            if (!attempt_status.empty()) attempt_status += "; ";
            attempt_status += name;
            attempt_status += ": ";
            attempt_status += reason;
        };

        for (const ContextAttempt& attempt : attempts) {
            QSurfaceFormat requested;
            requested.setRenderableType(QSurfaceFormat::OpenGL);
            requested.setVersion(3, 3);
            requested.setProfile(attempt.profile);
            requested.setDepthBufferSize(0);
            requested.setStencilBufferSize(0);
            requested.setSamples(0);

            context_ = new QOpenGLContext;
            if (QGuiApplication::primaryScreen() != nullptr) {
                context_->setScreen(QGuiApplication::primaryScreen());
            }
            context_->setFormat(requested);
            if (!context_->create()) {
                record_failure(attempt.name, "context creation failed");
                discard_candidate();
                continue;
            }

            const QSurfaceFormat actual = context_->format();
            if (actual.renderableType() != QSurfaceFormat::OpenGL
                || actual.majorVersion() < 3
                || (actual.majorVersion() == 3
                    && actual.minorVersion() < 3)) {
                record_failure(attempt.name,
                               "the driver returned less than OpenGL 3.3");
                discard_candidate();
                continue;
            }

            // QOffscreenSurface must use the context's negotiated format, not
            // the requested approximation. Windows WGL drivers commonly
            // adjust the format, and pairing the original request with that
            // context can make an otherwise valid GPU appear unavailable.
            surface_ = new QOffscreenSurface;
            if (context_->screen() != nullptr) {
                surface_->setScreen(context_->screen());
            }
            surface_->setFormat(actual);
            surface_->create();
            if (!surface_->isValid()) {
                record_failure(attempt.name,
                               "matching offscreen surface creation failed");
                discard_candidate();
                continue;
            }
            if (!context_->makeCurrent(surface_)) {
                record_failure(attempt.name,
                               "the matching context could not be made current");
                discard_candidate();
                continue;
            }

            QOpenGLExtraFunctions* functions = context_->extraFunctions();
            functions->initializeOpenGLFunctions();
            vendor = gl_text(functions->glGetString(GL_VENDOR));
            renderer = gl_text(functions->glGetString(GL_RENDERER));
            version = gl_text(functions->glGetString(GL_VERSION));
            context_->doneCurrent();
            selected_attempt = attempt.name;
            break;
        }

        if (context_ == nullptr || surface_ == nullptr) {
            status_ = "Qt could not create a usable desktop OpenGL 3.3 "
                      "context";
            if (!attempt_status.empty()) status_ += " (" + attempt_status + ")";
            status_ += ".";
            return;
        }

        device_name_ = renderer.empty() ? vendor : renderer;
        if (device_name_.empty()) device_name_ = "unnamed OpenGL device";

        worker_ = new QObject;
        const bool threaded = QOpenGLContext::supportsThreadedOpenGL();
        if (threaded) {
            render_thread_ = new QThread;
            owns_render_thread_ = true;
            context_->moveToThread(render_thread_);
            worker_->moveToThread(render_thread_);
            render_thread_->start();
        } else {
            // Some Windows ICDs expose valid hardware OpenGL but explicitly do
            // not support a context on a secondary thread. Keeping the context
            // on Qt's GUI thread is slower than the dedicated path but still
            // preserves acceleration instead of disabling it outright.
            render_thread_ = QThread::currentThread();
        }
        ready_ = true;
        status_ = "OpenGL analytic-surface acceleration is ready on "
                  + device_name_;
        if (!version.empty()) status_ += " (" + version + ")";
        if (selected_attempt != "core profile") {
            status_ += " using the " + selected_attempt + " fallback";
        }
        if (!threaded) {
            status_ += " on Qt's GUI thread because this driver does not "
                       "support threaded OpenGL";
        }
        status_ += ".";
    }

    bool compile_shader(QOpenGLExtraFunctions* gl, GLenum type,
                        const char* source, GLuint& shader,
                        std::string* error) {
        shader = gl->glCreateShader(type);
        if (shader == 0U) return fail(error, "OpenGL could not create a shader.");
        gl->glShaderSource(shader, 1, &source, nullptr);
        gl->glCompileShader(shader);
        GLint compiled = GL_FALSE;
        gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE) return true;
        GLint length = 0;
        gl->glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::vector<char> log(static_cast<std::size_t>((std::max)(1, length)));
        gl->glGetShaderInfoLog(shader, length, nullptr, log.data());
        return fail(error, std::string("OpenGL shader compilation failed: ")
                               + log.data());
    }

    bool ensure_program(QOpenGLExtraFunctions* gl, std::string* error) {
        if (program_ != 0U) return true;
        GLuint vertex = 0U;
        GLuint fragment = 0U;
        if (!compile_shader(gl, GL_VERTEX_SHADER, kVertexShader, vertex, error)
            || !compile_shader(gl, GL_FRAGMENT_SHADER, kFragmentShader,
                               fragment, error)) {
            if (vertex != 0U) gl->glDeleteShader(vertex);
            if (fragment != 0U) gl->glDeleteShader(fragment);
            return false;
        }
        program_ = gl->glCreateProgram();
        if (program_ == 0U) {
            gl->glDeleteShader(vertex);
            gl->glDeleteShader(fragment);
            return fail(error, "OpenGL could not create a shader program.");
        }
        gl->glAttachShader(program_, vertex);
        gl->glAttachShader(program_, fragment);
        gl->glLinkProgram(program_);
        gl->glDeleteShader(vertex);
        gl->glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        gl->glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            GLint length = 0;
            gl->glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(
                static_cast<std::size_t>((std::max)(1, length)));
            gl->glGetProgramInfoLog(program_, length, nullptr, log.data());
            const std::string message =
                std::string("OpenGL shader linking failed: ") + log.data();
            gl->glDeleteProgram(program_);
            program_ = 0U;
            return fail(error, message);
        }
        gl->glGenVertexArrays(1, &vertex_array_);
        return vertex_array_ != 0U
                   || fail(error, "OpenGL could not create a vertex array.");
    }

    bool render_on_gpu_thread(const Image& source, Image& destination,
                              const SurfaceConfig& surface, double loop_phase,
                              std::string* error) {
        std::size_t expected_values = 0U;
        const bool dimensions_fit = source.width > 0 && source.height > 0
            && static_cast<std::size_t>(source.width)
                   <= (std::numeric_limits<std::size_t>::max)()
                          / static_cast<std::size_t>(source.height)
            && (expected_values = static_cast<std::size_t>(source.width)
                                      * static_cast<std::size_t>(source.height))
                   <= (std::numeric_limits<std::size_t>::max)() / 4U;
        if (!dimensions_fit
            || source.pixels.size() != expected_values * 4U) {
            return fail(error, "OpenGL received inconsistent source image metadata.");
        }
        if (!context_->makeCurrent(surface_)) {
            return fail(error,
                        "OpenGL could not make its render context current.");
        }
        QOpenGLExtraFunctions* gl = context_->extraFunctions();
        gl->initializeOpenGLFunctions();
        while (gl->glGetError() != GL_NO_ERROR) {
            // Discard context-creation diagnostics before attributing an error
            // to this frame's transactional surface pass.
        }
        if (!ensure_program(gl, error)) {
            context_->doneCurrent();
            return false;
        }

        GLuint source_texture = 0U;
        GLuint destination_texture = 0U;
        GLuint framebuffer = 0U;
        gl->glGenTextures(1, &source_texture);
        gl->glBindTexture(GL_TEXTURE_2D, source_texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, source.width,
                         source.height, 0, GL_RGBA, GL_FLOAT,
                         source.pixels.data());

        gl->glGenTextures(1, &destination_texture);
        gl->glBindTexture(GL_TEXTURE_2D, destination_texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, source.width,
                         source.height, 0, GL_RGBA, GL_FLOAT, nullptr);

        gl->glGenFramebuffers(1, &framebuffer);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, destination_texture, 0);
        const GLenum framebuffer_status =
            gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (framebuffer_status != GL_FRAMEBUFFER_COMPLETE) {
            gl->glDeleteFramebuffers(1, &framebuffer);
            gl->glDeleteTextures(1, &destination_texture);
            gl->glDeleteTextures(1, &source_texture);
            context_->doneCurrent();
            return fail(error,
                        "OpenGL could not create a complete float-RGBA "
                        "surface framebuffer.");
        }

        gl->glViewport(0, 0, source.width, source.height);
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_DEPTH_TEST);
        gl->glUseProgram(program_);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, source_texture);
        gl->glUniform1i(gl->glGetUniformLocation(program_, "sourceImage"), 0);
        gl->glUniform2i(gl->glGetUniformLocation(program_, "imageSize"),
                        source.width, source.height);
        gl->glUniform1i(gl->glGetUniformLocation(program_, "mapping"),
                        static_cast<int>(surface.mapping));
        gl->glUniform1i(gl->glGetUniformLocation(program_, "projection"),
                        static_cast<int>(surface.projection));
        gl->glUniform1i(gl->glGetUniformLocation(program_, "sizing"),
                        static_cast<int>(surface.sizing));
        gl->glUniform1i(gl->glGetUniformLocation(program_, "outsideMode"),
                        static_cast<int>(surface.outside));
        gl->glUniform1i(
            gl->glGetUniformLocation(program_, "compositeBackfaces"),
            surface.composite_backfaces ? 1 : 0);
        gl->glUniform1i(gl->glGetUniformLocation(program_, "rotationOrder"),
                        static_cast<int>(surface.rotation_order));
        constexpr double pi = 3.141592653589793238462643383279502884;
        gl->glUniform3f(
            gl->glGetUniformLocation(program_, "rotation"),
            static_cast<float>(surface.rotation_x_degrees * pi / 180.0
                + static_cast<double>(surface.rotation_x_turns_per_loop)
                      * loop_phase),
            static_cast<float>(surface.rotation_y_degrees * pi / 180.0
                + static_cast<double>(surface.rotation_y_turns_per_loop)
                      * loop_phase),
            static_cast<float>(surface.rotation_z_degrees * pi / 180.0
                + static_cast<double>(surface.rotation_z_turns_per_loop)
                      * loop_phase));
        gl->glUniform3f(gl->glGetUniformLocation(program_, "objectScale"),
                        static_cast<float>(surface.scale_x),
                        static_cast<float>(surface.scale_y),
                        static_cast<float>(surface.scale_z));
        gl->glUniform3f(gl->glGetUniformLocation(program_, "position"),
                        static_cast<float>(surface.position_x_percent),
                        static_cast<float>(surface.position_y_percent),
                        static_cast<float>(surface.position_z));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "sizeMultiplier"),
                        static_cast<float>(surface.size_percent / 100.0));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "cameraDistance"),
                        static_cast<float>(surface.camera_distance));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "focalLength"),
                        static_cast<float>(surface.focal_length));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "curvature"),
                        static_cast<float>((std::max)(
                            0.0, (std::min)(1.0, surface.curvature))));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "lighting"),
                        static_cast<float>(surface.lighting));
        gl->glUniform3f(gl->glGetUniformLocation(program_, "lightDirection"),
                        static_cast<float>(surface.light_direction_x),
                        static_cast<float>(surface.light_direction_y),
                        static_cast<float>(surface.light_direction_z));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "lightAmbient"),
                        static_cast<float>(surface.light_ambient));
        gl->glUniform1f(gl->glGetUniformLocation(program_, "lightDiffuse"),
                        static_cast<float>(surface.light_diffuse));
        gl->glBindVertexArray(vertex_array_);
        gl->glDrawArrays(GL_TRIANGLES, 0, 3);

        destination.width = source.width;
        destination.height = source.height;
        destination.pixels.resize(source.pixels.size());
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glReadPixels(0, 0, source.width, source.height, GL_RGBA, GL_FLOAT,
                         destination.pixels.data());
        const GLenum render_status = gl->glGetError();

        gl->glBindFramebuffer(GL_FRAMEBUFFER, 0U);
        gl->glDeleteFramebuffers(1, &framebuffer);
        gl->glDeleteTextures(1, &destination_texture);
        gl->glDeleteTextures(1, &source_texture);
        context_->doneCurrent();
        if (render_status != GL_NO_ERROR) {
            return fail(error, "OpenGL surface rendering failed with error "
                                   + std::to_string(render_status) + ".");
        }

        const std::size_t row_values =
            static_cast<std::size_t>(source.width) * 4U;
        for (int top = 0, bottom = source.height - 1; top < bottom;
             ++top, --bottom) {
            float* first = destination.pixels.data()
                           + static_cast<std::size_t>(top) * row_values;
            float* second = destination.pixels.data()
                            + static_cast<std::size_t>(bottom) * row_values;
            std::swap_ranges(first, first + row_values, second);
        }
        return true;
    }

    std::mutex mutex_;
    bool initialization_attempted_ = false;
    bool ready_ = false;
    std::string device_name_;
    std::string status_ = "OpenGL surface acceleration has not been initialized.";
    QOffscreenSurface* surface_ = nullptr;
    QOpenGLContext* context_ = nullptr;
    QThread* render_thread_ = nullptr;
    bool owns_render_thread_ = false;
    QObject* worker_ = nullptr;
    GLuint program_ = 0U;
    GLuint vertex_array_ = 0U;
};

OpenGLSurfaceService* g_service_instance = nullptr;

void shutdown_service() {
    if (g_service_instance != nullptr) g_service_instance->shutdown();
}

OpenGLSurfaceService& service() {
    // Keep the C++ service itself alive through static destruction, but release
    // Qt and native GL objects from QCoreApplication's GUI-thread destructor
    // hook while the platform integration still exists.
    static OpenGLSurfaceService* instance = [] {
        auto* result = new OpenGLSurfaceService;
        g_service_instance = result;
        qAddPostRoutine(shutdown_service);
        return result;
    }();
    return *instance;
}

bool surface_has_supported_work(const SurfaceConfig& surface) {
    if (!surface.enabled || surface.mapping == SurfaceMapping::CustomObj) {
        return false;
    }
    if (surface.mapping == SurfaceMapping::Plane) {
        return surface.projection != SurfaceProjection::Orthographic
               || surface.sizing != SurfaceSizing::Contain
               || surface.rotation_x_turns_per_loop != 0
               || surface.rotation_y_turns_per_loop != 0
               || surface.rotation_z_turns_per_loop != 0
               || std::fmod(surface.rotation_x_degrees, 360.0) != 0.0
               || std::fmod(surface.rotation_y_degrees, 360.0) != 0.0
               || std::fmod(surface.rotation_z_degrees, 360.0) != 0.0
               || surface.size_percent != 100.0
               || surface.scale_x != 1.0 || surface.scale_y != 1.0
               || surface.scale_z != 1.0
               || surface.position_x_percent != 0.0
               || surface.position_y_percent != 0.0
               || surface.position_z != 0.0 || surface.lighting != 0.0;
    }
    return surface.curvature > 0.0;
}

} // namespace

bool opengl_surface_backend_compiled() {
    return true;
}

bool opengl_surface_backend_available(std::string* device_name,
                                      std::string* status) {
    return service().available(device_name, status);
}

bool opengl_surface_backend_supports(const RenderConfig& config,
                                     std::string* reason) {
    if (surface_has_supported_work(config.surface)) {
        if (reason != nullptr) reason->clear();
        return true;
    }
    if (reason != nullptr) {
        if (!config.surface.enabled) {
            *reason = "Strict OpenGL GPU rendering requires an active analytic "
                      "surface mapping.";
        } else if (config.surface.mapping == SurfaceMapping::CustomObj) {
            *reason = "Strict OpenGL GPU rendering does not accelerate imported "
                      "OBJ mesh rasterization.";
        } else if (config.surface.mapping == SurfaceMapping::Plane
                   && config.surface.plane_displacement.enabled
                   && config.surface.curvature > 0.0) {
            *reason = "Strict OpenGL GPU rendering does not accelerate "
                      "displacement-Plane mesh rasterization.";
        } else if (config.surface.mapping == SurfaceMapping::Plane) {
            *reason = "Strict OpenGL GPU rendering requires an active flat "
                      "Plane transform; displacement-Plane meshes remain "
                      "ordered CPU stages.";
        } else {
            *reason = "Strict OpenGL GPU rendering requires an active analytic "
                      "Cylinder, Sphere, or Cube surface mapping.";
        }
    }
    return false;
}

bool opengl_surface_backend_supports(const SurfaceConfig& surface) {
    return surface_has_supported_work(surface);
}

bool opengl_surface_acceleration_active() {
    return g_surface_acceleration_active;
}

bool set_opengl_surface_acceleration_active(bool active) {
    const bool previous = g_surface_acceleration_active;
    g_surface_acceleration_active = active;
    return previous;
}

bool apply_surface_mapping_opengl(const Image& source, Image& destination,
                                  const SurfaceConfig& surface,
                                  double loop_phase,
                                  const std::atomic_bool* cancel,
                                  std::string* error) {
    return service().render(source, destination, surface, loop_phase, cancel,
                            error);
}

} // namespace pvt::detail
