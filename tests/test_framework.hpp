#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace test {

class Failure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Suite {
public:
    template <typename Function>
    void run(std::string name, Function&& function) {
        try {
            std::forward<Function>(function)();
            ++passed_;
            std::cout << "PASS  " << name << '\n';
        } catch (const std::exception& error) {
            ++failed_;
            std::cerr << "FAIL  " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed_;
            std::cerr << "FAIL  " << name << ": unknown exception\n";
        }
    }

    void expect(bool condition, std::string message) const {
        if (!condition) {
            throw Failure{std::move(message)};
        }
    }

    template <typename Actual, typename Expected>
    void equal(const Actual& actual, const Expected& expected, std::string message) const {
        if (!(actual == expected)) {
            throw Failure{std::move(message)};
        }
    }

    [[nodiscard]] int finish() const {
        std::cout << "\n" << passed_ << " tests passed, " << failed_ << " failed\n";
        return failed_ == 0 ? 0 : 1;
    }

private:
    int passed_{0};
    int failed_{0};
};

}  // namespace test
