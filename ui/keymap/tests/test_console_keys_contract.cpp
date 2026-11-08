// Contract test for ADR-0070: the declared types compile and the fake keeps
// what a test puts in it.
#include <cstdio>
#include <type_traits>

#include "sonora/console_keys_fake.h"

using namespace sonora;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// The declared signatures; linking is not required, only that they compile.
static_assert(std::is_same_v<decltype(&parse_console_keymap),
    ConsoleKeymap (*)(std::string_view, std::vector<std::string>*)>);
static_assert(std::is_same_v<decltype(&builtin_console_keymap),
    ConsoleKeymap (*)()>);
static_assert(std::is_same_v<decltype(&filter_console_chords),
    std::vector<KeyChord> (*)(const std::vector<KeyChord>&,
                              const ConsoleKeymap&)>);

int main() {
    ConsoleKeymap us = fake::default_us();
    CHECK(!us.empty());
    CHECK(us.rows.size() == 4);
    CHECK(us.rows[0].keycode == 59);
    CHECK(us.rows[0].columns[4] == "Console_1");
    CHECK(us.declared_masks.size() == 5);

    ConsoleKeymap re = fake::remapped();
    CHECK(re.rows[1].columns[4] == "Meta_F2");
    CHECK(us.rows[1].columns[4] == "Console_2");  // copy, not alias

    CHECK(ConsoleKeymap{}.empty());
    return failures ? 1 : 0;
}
