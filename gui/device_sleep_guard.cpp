#include "device_sleep_guard.h"

#if defined(__APPLE__)
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

DeviceSleepGuard::~DeviceSleepGuard() {
    (void)setPrevented(false, nullptr);
}

bool DeviceSleepGuard::setPrevented(bool prevented, QString* error) {
    if (error != nullptr) error->clear();
#if defined(__APPLE__)
    if (!prevented) {
        if (assertion_id_ != kIOPMNullAssertionID) {
            IOPMAssertionRelease(assertion_id_);
            assertion_id_ = kIOPMNullAssertionID;
        }
        return true;
    }
    if (assertion_id_ != kIOPMNullAssertionID) return true;
    IOPMAssertionID created = kIOPMNullAssertionID;
    const IOReturn result = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn,
        CFSTR("Procedural Visualizer Tool live performance"), &created);
    if (result != kIOReturnSuccess) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "macOS rejected the Live-mode sleep-prevention request (%1).")
                         .arg(static_cast<unsigned int>(result));
        }
        return false;
    }
    assertion_id_ = created;
    return true;
#elif defined(_WIN32)
    const EXECUTION_STATE requested = prevented
        ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
        : ES_CONTINUOUS;
    if (SetThreadExecutionState(requested) == 0) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "Windows rejected the Live-mode sleep-prevention request.");
        }
        return false;
    }
    prevented_ = prevented;
    return true;
#else
    if (prevented && error != nullptr) {
        *error = QStringLiteral(
            "Sleep prevention is not available on this platform build.");
    }
    return !prevented;
#endif
}

bool DeviceSleepGuard::isPrevented() const noexcept {
#if defined(__APPLE__)
    return assertion_id_ != kIOPMNullAssertionID;
#elif defined(_WIN32)
    return prevented_;
#else
    return false;
#endif
}
