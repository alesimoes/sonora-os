// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Output page of Setup > I/O Setup: output paths (ADR-0060), the aux
// buses that play on a pair of the device's outputs. See io_setup_model.h.
#include <QVariantMap>
#include <array>

#include "io_setup_model.h"
#include "session_model.h"
#include "sonora/model/operations.h"
#include "timeline_document.h"

namespace sonora::ui {

using namespace model;

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }

std::string trimmed(const std::string& name) {
    std::string n = name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(n.begin());
    return n;
}

}  // namespace

QString IoSetupModel::pairText(int first) const {
    return q(names_.name(IoSide::Output, static_cast<std::size_t>(first))) + QStringLiteral(" + ") +
           q(names_.name(IoSide::Output, static_cast<std::size_t>(first + 1)));
}

void IoSetupModel::buildPaths(QVariantList& paths, QVariantList& pairs) const {
    const Snapshot& s = doc_.snapshot();
    const std::array<std::uint16_t, 2>& master = s.mix.bus.outputs;

    int row = 0;
    for (const Bus& b : s.edit->buses) {
        if (b.hardware == kNoHardwareOut) continue;
        int strips = 0;
        for (const auto& t : s.edit->tracks) strips += t->output == b.id ? 1 : 0;
        for (const Bus& other : s.edit->buses) strips += other.output == b.id ? 1 : 0;
        const int first = b.hardware;
        QVariantMap m;
        m[QStringLiteral("row")] = row++;
        m[QStringLiteral("name")] = q(b.name);
        m[QStringLiteral("pair")] = first;
        m[QStringLiteral("strips")] = strips;
        const bool there = output_count_ > 0 && first + 2 <= output_count_;
        m[QStringLiteral("connected")] = there;
        m[QStringLiteral("pairText")] = there ? pairText(first)
                                              : QStringLiteral("outputs %1-%2").arg(first + 1).arg(first + 2);
        m[QStringLiteral("note")] =
            there ? QString()
                  : output_count_ <= 0
                        ? QStringLiteral("not known: the engine is not answering")
                        : QStringLiteral("not connected: the interface has %1 output%2")
                              .arg(output_count_)
                              .arg(output_count_ == 1 ? QString() : QStringLiteral("s"));
        paths << m;
    }

    for (int first = 0; first + 2 <= output_count_; first += 2) {
        QString by;
        if (master[0] / 2 == first / 2 || master[1] / 2 == first / 2) {
            by = QStringLiteral("Master");
        }
        for (const Bus& b : s.edit->buses) {
            if (b.hardware == first) by = q(b.name);
        }
        QVariantMap m;
        m[QStringLiteral("pair")] = first;
        m[QStringLiteral("text")] = pairText(first);
        m[QStringLiteral("taken")] = !by.isEmpty();
        m[QStringLiteral("by")] = by;
        pairs << m;
    }
}

const Bus* IoSetupModel::pathAt(int row, std::string& why) const {
    int at = 0;
    for (const Bus& b : doc_.snapshot().edit->buses) {
        if (b.hardware == kNoHardwareOut) continue;
        if (at++ == row) return &b;
    }
    why = "that output path is gone";
    return nullptr;
}

bool IoSetupModel::pairProblem(int pair, std::string& why) const {
    if (output_count_ <= 0) {
        why = "the engine is not answering, so which outputs this interface has is not known";
        return true;
    }
    if (pair < 0 || pair % 2 != 0 || pair + 2 > output_count_) {
        why = "there is no pair of outputs starting at output " + std::to_string(pair + 1) +
              "; the interface has " + std::to_string(output_count_);
        return true;
    }
    return false;
}

bool IoSetupModel::makePath(const std::string& raw, int pair, std::string& why) {
    refresh();
    if (pairProblem(pair, why)) return false;
    std::string name = trimmed(raw);
    if (name.empty()) name = "Out " + std::to_string(pair + 1) + "-" + std::to_string(pair + 2);
    Compound c;
    c.label = "New Output Path";
    AddBus bus;
    bus.bus = doc_.reserve_id();
    bus.name = name;
    c.ops.push_back(bus);
    c.ops.push_back(SetBusPath{bus.bus, name, static_cast<std::uint16_t>(pair)});
    const bool ok = doc_.submit(c, why);
    session_.reportEdit(ok, why);
    refresh();
    return ok;
}

bool IoSetupModel::movePath(int row, int pair, std::string& why) {
    refresh();
    const Bus* b = pathAt(row, why);
    if (b == nullptr || pairProblem(pair, why)) return false;
    if (b->hardware == pair) return true;
    const bool ok = doc_.submit(SetBusPath{b->id, b->name, static_cast<std::uint16_t>(pair)}, why);
    session_.reportEdit(ok, why);
    refresh();
    return ok;
}

bool IoSetupModel::namePath(int row, const std::string& raw, std::string& why) {
    refresh();
    const Bus* b = pathAt(row, why);
    if (b == nullptr) return false;
    const std::string name = trimmed(raw);
    if (name.empty()) {
        why = "an output path needs a name";
        return false;
    }
    for (const Bus& other : doc_.snapshot().edit->buses) {
        if (other.id != b->id && other.name == name) {
            why = "a bus is already called \"" + name + "\"";
            return false;
        }
    }
    if (name == b->name) return true;
    const bool ok = doc_.submit(SetBusPath{b->id, name, b->hardware}, why);
    session_.reportEdit(ok, why);
    refresh();
    return ok;
}

bool IoSetupModel::dropPath(int row, std::string& why) {
    refresh();
    const Bus* b = pathAt(row, why);
    if (b == nullptr) return false;
    const EditState& e = *doc_.snapshot().edit;
    const Id id = b->id;
    Compound c;
    c.label = "Remove Output Path";
    // A strip routed to the path goes back to the Master in the same step:
    // the model refuses the removal while one is routed to it.
    for (const auto& t : e.tracks) {
        if (t->output == id) c.ops.push_back(SetOutput{t->id, kMasterOutput});
    }
    for (const Bus& other : e.buses) {
        if (other.output == id) c.ops.push_back(SetOutput{other.id, kMasterOutput});
    }
    const auto feeding = [&](const std::vector<Send>& sends) {
        for (const Send& sd : sends) {
            if (sd.bus == id) c.ops.push_back(RemoveSend{sd.id});
        }
    };
    for (const auto& t : e.tracks) feeding(t->sends);
    for (const Bus& other : e.buses) feeding(other.sends);
    c.ops.push_back(RemoveBus{id});
    const bool ok = doc_.submit(c, why);
    session_.reportEdit(ok, why);
    refresh();
    return ok;
}

bool IoSetupModel::addPath(const QString& name, int pair) {
    std::string why;
    return done(makePath(name.toStdString(), pair, why), why);
}

bool IoSetupModel::setPathPair(int row, int pair) {
    std::string why;
    return done(movePath(row, pair, why), why);
}

bool IoSetupModel::renamePath(int row, const QString& name) {
    std::string why;
    return done(namePath(row, name.toStdString(), why), why);
}

bool IoSetupModel::removePath(int row) {
    std::string why;
    return done(dropPath(row, why), why);
}

}  // namespace sonora::ui
