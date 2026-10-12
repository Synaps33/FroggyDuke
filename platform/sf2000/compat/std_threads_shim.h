/*
 * Single-threaded stand-ins for the std:: types the console's libstdc++ omits.
 *
 * The console toolchain (mips-mti-elf, CodeWarrior newlib) is configured with
 * --disable-libgomp and a single thread model, so libstdc++ leaves
 * _GLIBCXX_HAS_GTHREADS undefined. Everything in <bits/std_mutex.h> that
 * depends on it simply does not exist there: std::mutex, std::recursive_mutex,
 * std::timed_mutex, std::condition_variable*, and all of <thread>.
 *
 * (std::atomic, std::lock_guard, std::unique_lock and the lock tag types *are*
 * present -- they live outside that guard -- so this header must not define
 * them, or it collides with the real ones.)
 *
 * eduke32 needs them because libasync.h -- the task system its audio decoders
 * are built on (multivoc, vorbis, xmp, flac) -- is written against them.
 *
 * The console has exactly one thread, so every one of these has a single
 * correct implementation here: the lock is already held and there is nobody to
 * wait for. These are not placeholders for an implementation that never got
 * written; on this target they are the right answer.
 *
 * std::thread is the one genuinely unavailable piece. libasync constructs one
 * only inside recursive_spawn(), which spins up a real worker pool, and
 * eduke32 never calls it -- it uses plain async::spawn(), whose tasks are
 * pumped by when_all()/when_any() on the calling thread. So the stub never
 * runs anything, and a future caller reaching recursive_spawn() would get a
 * non-joinable thread rather than a second thread of execution.
 *
 * Force-included for SF2000 builds (see Makefile.sf2000) because libasync.h
 * includes <mutex> and <thread> itself, and there is no way to make those
 * headers emit what this libstdc++ does not have.
 */
#ifndef SF2K_STD_THREADS_SHIM_H
#define SF2K_STD_THREADS_SHIM_H

/* thread_local -> plain static.
 *
 * There is one thread on this target, so per-thread storage is per-program
 * storage and the distinction cannot be observed. It has to be removed rather
 * than merely unused, because GCC lowers C++ TLS on this target to
 * "rdhwr $rt, $CP0_ULR" (rdhwr $a1, $29) to find the thread base. The JZ4775
 * does not implement that instruction and raises a Reserved Instruction
 * exception, which took the core down before main() -- in sm::GetTlsBucket(),
 * the first thing smmalloc_tls.cpp calls. smmalloc keeps the non-TLS variant
 * of the same array commented out right below the declaration, which is
 * exactly what this macro turns it into. */
#define thread_local

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace std {

/*
 * std::to_string.
 *
 * This libstdc++'s <string> omits the to_string family entirely: those
 * overloads are guarded on the C99 stdio being available, and the bare-metal
 * newlib here is not configured that way. Several sources use them (loguru's
 * error-context formatting, for one), so define them over snprintf, which
 * newlib does provide.
 */
namespace detail {

/* One entry point, told how to print. A template rather than two overloads so
 * that an int argument cannot be ambiguous between them. */
template <typename T>
inline string to_string_impl(T v, const char *fmt)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), fmt, static_cast<long long>(v));
    return string(buf, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : (size_t)0);
}

template <typename T>
inline string to_string_impl_u(T v, const char *fmt)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), fmt, static_cast<unsigned long long>(v));
    return string(buf, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : (size_t)0);
}

} // namespace detail

inline string to_string(int v)                  { return detail::to_string_impl<long long>(v, "%lld"); }
inline string to_string(unsigned int v)         { return detail::to_string_impl_u<unsigned long long>(v, "%llu"); }
inline string to_string(long v)                 { return detail::to_string_impl<long long>(v, "%lld"); }
inline string to_string(unsigned long v)        { return detail::to_string_impl_u<unsigned long long>(v, "%llu"); }
inline string to_string(long long v)            { return detail::to_string_impl<long long>(v, "%lld"); }
inline string to_string(unsigned long long v)   { return detail::to_string_impl_u<unsigned long long>(v, "%llu"); }
inline string to_string(short v)                { return to_string(static_cast<int>(v)); }
inline string to_string(unsigned short v)       { return to_string(static_cast<unsigned int>(v)); }

inline string to_string(float v)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return string(buf, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : 0u);
}

inline string to_string(double v)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%g", v);
    return string(buf, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : 0u);
}

inline string to_string(long double v)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%Lg", v);
    return string(buf, (n > 0 && (size_t)n < sizeof(buf)) ? (size_t)n : 0u);
}

inline string to_string(bool v) { return v ? "true" : "false"; }

inline string to_string(const char *v) { return string(v ? v : ""); }

/* libstdc++ does provide unique_lock, but only once <mutex> has been included
 * -- and this header is force-included ahead of everything else. Declare it so
 * condition_variable below can name it. */
template <typename _Mutex>
class unique_lock;

/* ---------------------------------------------------------------- locks */

class mutex
{
public:
    constexpr mutex() noexcept = default;
    ~mutex() = default;

    mutex(const mutex &) = delete;
    mutex &operator=(const mutex &) = delete;

    void lock() noexcept {}
    bool try_lock() noexcept { return true; }
    void unlock() noexcept {}
};

class recursive_mutex
{
public:
    constexpr recursive_mutex() noexcept = default;
    ~recursive_mutex() = default;

    recursive_mutex(const recursive_mutex &) = delete;
    recursive_mutex &operator=(const recursive_mutex &) = delete;

    void lock() noexcept {}
    bool try_lock() noexcept { return true; }
    void unlock() noexcept {}
};

class timed_mutex : public recursive_mutex
{
public:
    constexpr timed_mutex() noexcept = default;

    template <typename Rep, typename Period>
    bool try_lock_for(const std::chrono::duration<Rep, Period> &) noexcept { return true; }

    template <typename Clock, typename Duration>
    bool try_lock_until(const std::chrono::time_point<Clock, Duration> &) noexcept { return true; }
};

/* ------------------------------------------------------------ condition */

class condition_variable
{
public:
    constexpr condition_variable() noexcept = default;
    ~condition_variable() = default;

    condition_variable(const condition_variable &) = delete;
    condition_variable &operator=(const condition_variable &) = delete;

    void notify_one() noexcept {}
    void notify_all() noexcept {}

    /* Nothing can ever change the predicate on a one-threaded system, so the
     * timed form reports success immediately rather than looping forever. */
    void wait(unique_lock<mutex> &) noexcept {}

    template <typename Rep, typename Period>
    bool wait_for(unique_lock<mutex> &, const std::chrono::duration<Rep, Period> &) noexcept
    {
        return true;
    }

    template <typename Clock, typename Duration>
    bool wait_until(unique_lock<mutex> &, const std::chrono::time_point<Clock, Duration> &) noexcept
    {
        return true;
    }

    template <typename Predicate>
    void wait(unique_lock<mutex> &lock, Predicate pred)
    {
        if (!pred())
            throw std::logic_error("SF2K_STD_THREADS_SHIM_H: predicate false with no other thread");
    }
};

class condition_variable_any : public condition_variable
{
public:
    constexpr condition_variable_any() noexcept = default;
};

/* --------------------------------------------------------------- thread */

class thread
{
public:
    struct id
    {
        id() noexcept = default;
        bool operator==(const id &) const noexcept { return true; }
        bool operator!=(const id &) const noexcept { return false; }
    };

    thread() noexcept = default;
    ~thread() = default;

    thread(const thread &) = delete;
    thread &operator=(const thread &) = delete;

    /* libasync's recursive_spawn() stores freshly constructed threads into an
     * array, so assignment from a temporary has to exist even though the thread
     * never runs anything. */
    thread(thread &&) noexcept = default;
    thread &operator=(thread &&) noexcept = default;

    template <typename F, typename... Args>
    explicit thread(F &&f, Args &&...args)
    {
        /* Deliberately does not run f -- see the file comment. Both parameters
         * are named so this compiles as C++14, which is what eduke32 uses and
         * so fold expressions are not available. */
        (void)f;
        (void)sizeof...(args);
    }

    bool joinable() const noexcept { return false; }
    void join() noexcept {}
    void detach() noexcept {}
    id get_id() const noexcept { return id(); }
    static unsigned int hardware_concurrency() noexcept { return 1; }
};

namespace this_thread {

inline void yield() noexcept {}

/* A real sleep, because libasync comes through here to let the other side of a
 * task have the CPU. */
template <typename Rep, typename Period>
inline void sleep_for(const std::chrono::duration<Rep, Period> &d) noexcept
{
    timespec ts;
    ts.tv_sec  = (time_t)(d.count() / 1000000000);
    ts.tv_nsec = (long)(d.count() % 1000000000);
    nanosleep(&ts, NULL);
}

template <typename Clock, typename Duration>
inline void sleep_until(const std::chrono::time_point<Clock, Duration> &tp) noexcept
{
    sleep_for(tp.time_since_epoch());
}

inline void sleep_for(const std::chrono::nanoseconds &d) noexcept
{
    timespec ts;
    ts.tv_sec  = (time_t)(d.count() / 1000000000);
    ts.tv_nsec = (long)(d.count() % 1000000000);
    nanosleep(&ts, NULL);
}

inline thread::id get_id() noexcept { return thread::id(); }

} // namespace this_thread

} // namespace std

namespace std {
/* loguru puts thread ids in an unordered_map keyed by thread::id. */
template <>
struct hash<thread::id>
{
    size_t operator()(const thread::id &) const noexcept { return 0; }
};
} // namespace std

#endif /* SF2K_STD_THREADS_SHIM_H */