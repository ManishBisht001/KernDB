#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kerndb::test {

using TestFunction = void (*)();

struct TestCase {
    std::string_view name;
    TestFunction function;
};

class TestRegistry {
public:
    static TestRegistry& Instance() {
        static TestRegistry registry;
        return registry;
    }

    void Register(std::string_view name, TestFunction function) {
        tests_.push_back(TestCase{
            .name = name,
            .function = function,
        });
    }

    int RunAll(std::ostream& output, std::ostream& error) const {
        std::size_t failures = 0U;
        for (const TestCase& test : tests_) {
            try {
                test.function();
                output << "[PASS] " << test.name << "\n";
            } catch (const std::exception& exception) {
                ++failures;
                error << "[FAIL] " << test.name << ": " << exception.what() << "\n";
            } catch (...) {
                ++failures;
                error << "[FAIL] " << test.name << ": unknown exception\n";
            }
        }

        output << tests_.size() - failures << "/" << tests_.size() << " tests passed\n";
        return failures == 0U ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
};

class Registrar {
public:
    Registrar(std::string_view name, TestFunction function) {
        TestRegistry::Instance().Register(name, function);
    }
};

[[noreturn]] inline void Fail(
    std::string_view expression,
    std::string_view file,
    int line) {
    throw std::runtime_error(
        std::string(file) + ":" + std::to_string(line) + ": expectation failed: " +
        std::string(expression));
}

}  // namespace kerndb::test

#define KERNDB_TEST(name)                                             \
    static void name();                                                \
    namespace {                                                        \
    const ::kerndb::test::Registrar k_registrar_##name{#name, &name}; \
    }                                                                  \
    static void name()

#define KERNDB_EXPECT(expression)                                  \
    do {                                                            \
        if (!(expression)) {                                        \
            ::kerndb::test::Fail(#expression, __FILE__, __LINE__); \
        }                                                           \
    } while (false)

#define KERNDB_EXPECT_EQ(expected, actual)                                      \
    do {                                                                         \
        const auto& k_expected_value = (expected);                              \
        const auto& k_actual_value = (actual);                                  \
        if (!(k_expected_value == k_actual_value)) {                            \
            ::kerndb::test::Fail(#expected " == " #actual, __FILE__, __LINE__); \
        }                                                                        \
    } while (false)
