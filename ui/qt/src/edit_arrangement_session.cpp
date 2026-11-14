// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SessionModel side of Sections in Memory Locations (ADR-0116): the
// rows, the destination and the held notice the window binds to.
#include "edit_arrangement.h"
#include "session_model.h"
#include "session_model_support.h"

namespace sonora::ui {

using namespace model;

QVariantList SessionModel::arrangementSections() const {
    const auto& settings = ctl_.snapshot().settings;
    QVariantList rows;
    int row = 0;
    for (const Marker& m : ctl_.arrangement_sections()) {
        QVariantMap r;
        r[QStringLiteral("row")] = row++;
        r[QStringLiteral("name")] = q(m.name);
        r[QStringLiteral("start")] = q(format_time(main_fmt_, m.position, settings));
        r[QStringLiteral("length")] = q(format_length(main_fmt_, m.position, m.length, settings));
        rows.push_back(r);
    }
    return rows;
}

QString SessionModel::arrangementDestination() const {
    return q(format_time(main_fmt_, ctl_.selection().insertion, ctl_.snapshot().settings));
}

bool SessionModel::arrangementApply(int row, bool move) {
    const std::vector<Marker> sections = ctl_.arrangement_sections();
    const SectionMode mode = move ? SectionMode::Move : SectionMode::Copy;
    std::string message;
    bool ok = false;
    if (row < 0 || static_cast<std::size_t>(row) >= sections.size()) {
        message = arrangement_refusal_message("That section is no longer in the session.", mode);
    } else {
        ok = ctl_.arrangement_apply(sections[static_cast<std::size_t>(row)].id,
                                    ctl_.selection().insertion, mode, message);
    }
    // Held in the window until the next try: the refusal must still be there
    // when the operator looks back, and the status line is gone in 2.5 s.
    arrangement_notice_ = q(message);
    emit arrangementNoticeChanged();
    sync();
    return ok;
}

}  // namespace sonora::ui
