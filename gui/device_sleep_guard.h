#ifndef PVT_DEVICE_SLEEP_GUARD_H
#define PVT_DEVICE_SLEEP_GUARD_H

#include <QString>

#include <cstdint>

class DeviceSleepGuard final {
public:
    DeviceSleepGuard() = default;
    ~DeviceSleepGuard();

    DeviceSleepGuard(const DeviceSleepGuard&) = delete;
    DeviceSleepGuard& operator=(const DeviceSleepGuard&) = delete;

    bool setPrevented(bool prevented, QString* error = nullptr);
    bool isPrevented() const noexcept;

private:
#if defined(__APPLE__)
    std::uint32_t assertion_id_ = 0U;
#elif defined(_WIN32)
    bool prevented_ = false;
#endif
};

#endif
