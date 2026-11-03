#include "test_support.hh"

#include <iostream>

namespace testing_support {

std::vector<Case> &registry()
{
    static std::vector<Case> cases;
    return cases;
}

Registrar::Registrar(const char *name, std::function<void()> body)
{
    registry().push_back(Case{name, std::move(body)});
}

} // namespace testing_support

int main()
{
    std::size_t failed = 0;
    for (const auto &test : testing_support::registry()) {
        try {
            test.body();
            std::cout << "PASS " << test.name << "\n";
        } catch (const std::exception &exception) {
            ++failed;
            std::cout << "FAIL " << test.name << "：" << exception.what() << "\n";
        }
    }
    std::cout << (failed == 0 ? "全部通过：" : "失败：") << failed << " / "
              << testing_support::registry().size() << "\n";
    return failed == 0 ? 0 : 1;
}
