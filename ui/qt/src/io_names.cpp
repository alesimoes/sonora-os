// SPDX-License-Identifier: GPL-3.0-or-later
#include "io_names.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace sonora::ui {

std::string default_io_name(IoSide side, std::size_t index) {
    return (side == IoSide::Input ? "In " : "Out ") + std::to_string(index + 1);
}

std::string IoNames::name(IoSide s, std::size_t index) const {
    const auto& m = side(s);
    const auto it = m.find(index);
    return it != m.end() ? it->second : default_io_name(s, index);
}

bool IoNames::renamed(IoSide s, std::size_t index) const { return side(s).count(index) != 0; }

std::string IoNames::problem(IoSide s, std::size_t index, std::size_t count,
                             const std::string& n) const {
    const char* what = s == IoSide::Input ? "an input" : "an output";
    if (index >= count) {
        return "there is no " + std::string(s == IoSide::Input ? "input " : "output ") +
               std::to_string(index + 1) + " on this interface";
    }
    if (n.empty()) return std::string(what) + " needs a name";
    if (n.find_first_not_of(' ') == std::string::npos) return std::string(what) + " needs a name";
    if (n.size() > kMaxLength) {
        return "a name is at most " + std::to_string(kMaxLength) + " characters";
    }
    if (n.find_first_of("\t\r\n") != std::string::npos) return "a name is one line, with no tab";
    for (std::size_t i = 0; i < count; ++i) {
        if (i != index && name(s, i) == n) {
            return default_io_name(s, i) + " is already called \"" + n +
                   "\"; two of the same name would read the same in the input list";
        }
    }
    return std::string();
}

void IoNames::set(IoSide s, std::size_t index, const std::string& n) {
    auto& m = side(s);
    if (n == default_io_name(s, index)) {
        m.erase(index);
    } else {
        m[index] = n;
    }
}

std::string IoNames::serialize() const {
    std::string out;
    for (const auto& [i, n] : inputs_) out += "in\t" + std::to_string(i + 1) + "\t" + n + "\n";
    for (const auto& [i, n] : outputs_) out += "out\t" + std::to_string(i + 1) + "\t" + n + "\n";
    return out;
}

IoNames IoNames::parse(const std::string& text) {
    IoNames names;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t a = line.find('\t');
        if (a == std::string::npos) continue;
        const std::size_t b = line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::string kind = line.substr(0, a);
        const std::string number = line.substr(a + 1, b - a - 1);
        const std::string n = line.substr(b + 1);
        IoSide s;
        if (kind == "in") {
            s = IoSide::Input;
        } else if (kind == "out") {
            s = IoSide::Output;
        } else {
            continue;
        }
        if (number.empty() || number.size() > 4 ||
            number.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const int k = std::atoi(number.c_str());
        if (k < 1 || n.empty() || n.size() > kMaxLength) continue;
        names.set(s, static_cast<std::size_t>(k - 1), n);
    }
    return names;
}

std::string io_names_path(const std::string& config_home) {
    std::string base = config_home;
    if (base.empty()) {
        const char* x = std::getenv("XDG_CONFIG_HOME");
        if (x != nullptr && fs::path(x).is_absolute()) base = x;
    }
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') return std::string();
        base = (fs::path(home) / ".config").string();
    }
    return (fs::path(base) / "sonora" / "io-names").string();
}

IoNames load_io_names(const std::string& path) {
    if (path.empty()) return IoNames();
    std::ifstream in(path, std::ios::binary);
    if (!in) return IoNames();
    std::ostringstream text;
    text << in.rdbuf();
    return IoNames::parse(text.str());
}

bool save_io_names(const std::string& path, const IoNames& names, std::string& why) {
    if (path.empty()) {
        why = "no configuration folder is known (neither XDG_CONFIG_HOME nor HOME is set)";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        why = "cannot create " + fs::path(path).parent_path().string() + ": " + ec.message();
        return false;
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << names.serialize();
        out.flush();
        if (!out) {
            why = "cannot write " + tmp;
            std::remove(tmp.c_str());
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        why = "cannot replace " + path + ": " + ec.message();
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace sonora::ui
