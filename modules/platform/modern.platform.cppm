module;

#include <chrono>
#include <cstddef>
#include <optional>
#include <thread>

export module modern.platform;

export namespace modern::platform
{
using steady_clock = std::chrono::steady_clock;
using steady_duration = steady_clock::duration;
using steady_time_point = steady_clock::time_point;
using thread_handle = std::thread::native_handle_type;

[[nodiscard]] steady_time_point now() noexcept;
void sleep_for(steady_duration duration);
void yield() noexcept;
[[nodiscard]] std::size_t hardware_concurrency() noexcept;
[[nodiscard]] std::optional<std::size_t> current_cpu() noexcept;
[[nodiscard]] bool set_thread_affinity(thread_handle thread, std::size_t cpu_index) noexcept;
}
