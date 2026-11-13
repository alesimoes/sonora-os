// SPDX-License-Identifier: GPL-3.0-or-later
// AafWriter seam over libaaf. libaaf (LGPL) is a read-only library: its author
// states it can neither modify nor create AAF files, and its public API has no
// writer. So even with SONORA_WITH_LIBAAF this writer refuses, loudly, rather
// than emit a file that is not AAF. A real backend replaces this class.
#include "aaf_libaaf_writer.h"

namespace sonora::interchange {

namespace {

class UnavailableWriter : public AafWriter {
public:
    bool write(const AafDocument&, const std::string&, std::string& error) override {
#ifdef SONORA_WITH_LIBAAF
        error = "AAF export unavailable: libaaf is read-only and has no AAF writer";
#else
        error = "AAF export unavailable: built without AAF support";
#endif
        return false;
    }
};

}  // namespace

std::unique_ptr<AafWriter> make_libaaf_writer() {
    return std::make_unique<UnavailableWriter>();
}

}  // namespace sonora::interchange
