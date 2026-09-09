#ifndef PVT_SCOPE_EXIT_H
#define PVT_SCOPE_EXIT_H

namespace pvt::detail {

// The callback must outlive this guard and must not throw. References keep
// cleanup allocation-free, including while handling allocation failures.
template <typename Callback>
class ScopeExit {
public:
    explicit ScopeExit(const Callback& callback) noexcept : callback_(callback) {}
    ~ScopeExit() { callback_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
private:
    const Callback& callback_;
};

template <typename Callback>
ScopeExit(const Callback&) -> ScopeExit<Callback>;

} // namespace pvt::detail

#endif
