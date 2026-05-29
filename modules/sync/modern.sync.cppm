module;

#include <chrono>
#include <condition_variable>
#include <mutex>

export module modern.sync;

import modern.platform;

export namespace modern::sync
{
class manual_reset_event
{
public:
	explicit manual_reset_event(bool signaled = false) noexcept;

	void set();
	void reset();
	void wait();
	[[nodiscard]] bool wait_until(platform::steady_time_point deadline);

	template<class Rep, class Period>
	[[nodiscard]] bool wait_for(std::chrono::duration<Rep, Period> timeout)
	{
		auto duration = std::chrono::duration_cast<platform::steady_duration>(timeout);
		if (duration <= platform::steady_duration::zero())
			return is_set();

		return wait_until(platform::now() + duration);
	}

	[[nodiscard]] bool is_set() const;

private:
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	bool signaled_;
};
}
