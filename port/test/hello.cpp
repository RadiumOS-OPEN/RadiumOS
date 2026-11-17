// First smoke test of the C++ runtime on RadiumOS: STL containers, exceptions, iostream, floats, 64-bit math.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

struct Thing { std::string name; int hp; };

static int risky(int depth)
{
    if (depth == 0) throw std::runtime_error("bottom reached");
    return risky(depth - 1) + 1;
}

int main()
{
    std::printf("hello from C++ on RadiumOS\n");
    std::vector<Thing> things = { {"zombie", 80}, {"survivor", 100}, {"dog", 30} };
    std::sort(things.begin(), things.end(), [](const Thing &a, const Thing &b) { return a.hp < b.hp; });
    std::map<std::string, int> ordered;
    std::unordered_map<std::string, int> hashed;
    for (auto &t : things) { ordered[t.name] = t.hp; hashed[t.name] = t.hp * 2; }
    for (auto &kv : ordered) std::cout << kv.first << "=" << kv.second << " (" << hashed[kv.first] << ")\n";

    try { risky(5); } catch (const std::exception &e) { std::cout << "caught: " << e.what() << "\n"; }

    std::ostringstream os;
    os << std::sqrt(2.0) << " " << std::sin(1.0) << " " << (long long)1 << 40;
    std::cout << os.str() << "\n";
    long long big = 1234567890123LL;
    std::printf("big/7=%lld mod=%lld pow=%.3f\n", big / 7, big % 7, std::pow(2.5, 3.5));
    auto up = std::make_unique<std::string>(std::string(40, 'x') + "!");
    std::cout << *up << "\n";
    auto t0 = std::chrono::steady_clock::now();
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "clock ok: " << (t1 >= t0) << "\n";
    return 0;
}
