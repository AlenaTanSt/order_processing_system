#pragma once

#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ops_test {

    struct Failure final : std::exception {
        std::string msg;
        explicit Failure(std::string m) : msg(std::move(m)) {}
        const char* what() const noexcept override { return msg.c_str(); }
    };

    struct TestCase final {
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<TestCase>& registry() {
        static std::vector<TestCase> r;
        return r;
    }

    inline void register_test(std::string name, std::function<void()> fn) {
        registry().push_back(TestCase{ std::move(name), std::move(fn) });
    }

    inline std::string format_loc(const char* file, int line) {
        std::ostringstream os;
        os << file << ":" << line;
        return os.str();
    }

    inline void require_impl(bool cond,
        const char* expr,
        const char* file,
        int line,
        const char* msg) {
        if (cond) return;

        std::ostringstream os;
        os << "FAIL: " << format_loc(file, line) << " - REQUIRE(" << expr << ")";
        if (msg && *msg) os << " - " << msg;
        throw Failure(os.str());
    }

    inline void fail_impl(const char* file, int line, const char* msg) {
        std::ostringstream os;
        os << "FAIL: " << format_loc(file, line);
        if (msg && *msg) os << " - " << msg;
        throw Failure(os.str());
    }

#define OPS_TEST_CONCAT_IMPL(a, b) a##b
#define OPS_TEST_CONCAT(a, b) OPS_TEST_CONCAT_IMPL(a, b)

#define OPS_TEST(name_literal)                                                     \
    static void OPS_TEST_CONCAT(ops_test_fn_, __LINE__)();                         \
    namespace {                                                                    \
    struct OPS_TEST_CONCAT(ops_test_reg_, __LINE__) {                              \
        OPS_TEST_CONCAT(ops_test_reg_, __LINE__)() {                               \
            ::ops_test::register_test((name_literal),                              \
                &OPS_TEST_CONCAT(ops_test_fn_, __LINE__));                         \
        }                                                                          \
    } OPS_TEST_CONCAT(ops_test_reg_instance_, __LINE__);                           \
    }                                                                              \
    static void OPS_TEST_CONCAT(ops_test_fn_, __LINE__)()

#define OPS_REQUIRE(cond) \
    ::ops_test::require_impl((cond), #cond, __FILE__, __LINE__, "")

#define OPS_REQUIRE_MSG(cond, message_literal) \
    ::ops_test::require_impl((cond), #cond, __FILE__, __LINE__, (message_literal))

#define OPS_FAIL(message_literal) \
    ::ops_test::fail_impl(__FILE__, __LINE__, (message_literal))

    inline bool contains_substr(const std::string& s, const std::string& sub) {
        return s.find(sub) != std::string::npos;
    }

    inline int run(int argc, char** argv) {
        const auto& tests = registry();

        bool list_only = false;
        std::string filter;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i] ? std::string(argv[i]) : std::string();

            if (arg == "--list") {
                list_only = true;
                continue;
            }

            const std::string pref = "--filter=";
            if (arg.rfind(pref, 0) == 0) {
                filter = arg.substr(pref.size());
                continue;
            }

            if (arg == "--filter") {
                if (i + 1 >= argc) {
                    std::cout << "FAIL: --filter requires a value\n";
                    return 2;
                }
                filter = argv[++i];
                continue;
            }
        }

        if (list_only) {
            for (const auto& t : tests) {
                std::cout << t.name << "\n";
            }
            return 0;
        }

        std::vector<const TestCase*> selected;
        selected.reserve(tests.size());

        if (filter.empty()) {
            for (const auto& t : tests) selected.push_back(&t);
        }
        else {
            for (const auto& t : tests) {
                if (contains_substr(t.name, filter)) selected.push_back(&t);
            }
        }

        std::cout << "Running " << selected.size() << " tests";
        if (!filter.empty()) std::cout << " (filter=\"" << filter << "\")";
        std::cout << "\n";

        int failed = 0;
        for (const auto* tp : selected) {
            const auto& t = *tp;
            try {
                t.fn();
                std::cout << "\"" << t.name << "\" - OK\n";
            }
            catch (const Failure& e) {
                ++failed;
                std::cout << "\"" << t.name << "\" - " << e.what() << "\n";
            }
            catch (const std::exception& e) {
                ++failed;
                std::cout << "\"" << t.name << "\" - FAIL: unexpected exception - " << e.what() << "\n";
            }
            catch (...) {
                ++failed;
                std::cout << "\"" << t.name << "\" - FAIL: unknown exception\n";
            }
        }

        if (failed == 0) {
            std::cout << "ALL OK\n";
            return 0;
        }

        std::cout << "FAILED: " << failed << " of " << selected.size() << "\n";
        return 1;
    }

    inline int run_all() {
        return run(1, nullptr);
    }

} // namespace ops_test