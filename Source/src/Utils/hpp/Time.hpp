#ifndef TIME_HPP
#define TIME_HPP

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

class Time {
 public:
  using Clock = std::chrono::high_resolution_clock;
  using TimePoint = std::chrono::time_point<Clock>;

  Time() = default;

  // Start a timer and return the current time point
  static TimePoint startTimer() {
    return Clock::now();
  }

  // Calculate elapsed time in seconds
  static double stopTimer(const TimePoint& start) {
    auto end = Clock::now();
    std::chrono::duration<double> duration = end - start;
    return duration.count();
  }

  // Get current time as a formatted string
  static std::string getCurrentTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::stringstream timeStream;
    timeStream << std::ctime(&now_time);
    return timeStream.str();
  }

  // Measure execution time of a function
  template <typename Func>
  static void measureExecutionTime(Func&& func) {
    TimePoint start = startTimer();
    func();  // Execute the function
    double executionTime = stopTimer(start);
    std::cout << "Total Execution Time: " << executionTime << " seconds" << std::endl;
  }
};

#endif  // TIME_HPP
