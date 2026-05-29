import modern.runtime;

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main()
{
  using namespace std::chrono_literals;

  modern::thread_pool cpu{4};
  modern::scheduled_executor timers{cpu};

  auto value = modern::submit(cpu, []
    {
      return 21;
    })
    .then([](int x)
    {
      return x * 2;
    })
    .finally([]
    {
      std::cout << "pipeline finished\n";
    })
    .get();

  std::cout << "value = " << value << "\n";

  auto recovered = modern::submit(cpu, []() -> int
    {
      throw std::runtime_error("boom");
    })
    .catching([](std::exception_ptr ep)
    {
      try
      {
        if (ep)
          std::rethrow_exception(ep);
      }
      catch (const std::exception& e)
      {
        std::cout << "recovered from: " << e.what() << "\n";
      }

      return 7;
    })
    .then([](int x)
    {
      return x + 1;
    })
    .get();

  std::cout << "recovered = " << recovered << "\n";

  auto delayed = modern::schedule_after(timers, 100ms, []
    {
      return std::string{"delayed result"};
    })
    .then([](std::string s)
    {
      return s + " + continuation";
    })
    .get();

  std::cout << delayed << "\n";

  int ticks = 0;
  auto periodic = timers.schedule_fixed_rate(0ms, 50ms, [&]
    {
      std::cout << "tick " << ++ticks << "\n";
    });

  modern::platform::sleep_for(180ms);
  periodic.request_stop();

  timers.shutdown();
  timers.join();
  cpu.shutdown();
  cpu.join();
}
