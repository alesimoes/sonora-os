// SPDX-License-Identifier: GPL-3.0-or-later
#include "sample_edit_detail.h"
#include "sonora/pitch_render.h"

namespace sonora::sample_edit_detail {

bool parse_pitch_verb(const std::string& param, SampleEditRequest& out, std::string& why) {
    if (out.channel != -1) {
        why = "pitch shifts every channel of the region; give all";
        return false;
    }
    if (param.empty()) {
        why = "pitch needs its notes as n=START:END:CENTS;n=...";
        return false;
    }
    return api::parse_pitch_param(param, out.pitch, why);
}

bool pitch_edit(model::Document& document, const Session& session, const std::string& session_dir,
                const SampleEditRequest& rq, std::vector<model::ReplicationUpdate>& updates,
                std::string& error) {
    return pitch_sample_edit(document, session, session_dir,
                             PitchRenderRequest{rq.region, rq.from, rq.to, rq.pitch}, updates, error);
}

}  // namespace sonora::sample_edit_detail
