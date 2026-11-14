// SPDX-License-Identifier: GPL-3.0-or-later
// AafReader over libaaf (LGPL-2.1-or-later). Compiled only with
// SONORA_WITH_LIBAAF. Each audio track becomes a composition slot; each
// distinct essence file becomes a source mob. Written against libaaf's
// AAF_Iface audio model (aafi_load_file, Audio->tracks, track->timelineItems).
#include <libaaf.h>

#include <map>

#include "sonora/interchange/aaf_import.h"

namespace sonora::interchange {

namespace {

AafRational rate_of(const aafRational_t& r) {
    return {r.numerator, r.denominator > 0 ? r.denominator : 1};
}

class LibAafReader : public AafReader {
public:
    bool read(const std::string& path, AafDocument& out, std::string& error) override {
        AAF_Iface* aafi = aafi_alloc(nullptr);
        if (!aafi) {
            error = "libaaf: out of memory";
            return false;
        }
        if (aafi_load_file(aafi, path.c_str()) != 0) {
            error = path + ": libaaf could not open the file as AAF";
            aafi_release(&aafi);
            return false;
        }
        if (!aafi->Audio) {
            error = path + ": the AAF holds no audio";
            aafi_release(&aafi);
            return false;
        }
        convert(aafi, out);
        aafi_release(&aafi);
        return true;
    }

private:
    static void convert(AAF_Iface* aafi, AafDocument& out) {
        AafMob comp;
        comp.mob_id = "composition";
        comp.name = "composition";
        std::map<std::string, std::size_t> sources;  // essence path -> index in out.mobs
        std::vector<AafMob> media;
        for (aafiAudioTrack* t = aafi->Audio->Tracks; t; t = t->next) {
            AafSlot slot;
            slot.name = t->name ? t->name : "";
            slot.edit_rate = rate_of(*t->edit_rate);
            std::int64_t cursor = 0;
            for (aafiTimelineItem* it = t->timelineItems; it; it = it->next) {
                if (it->type != AAFI_AUDIO_CLIP) {
                    out.unmapped.push_back("track " + slot.name + ": item other than a clip");
                    continue;
                }
                auto* clip = static_cast<aafiAudioClip*>(it->data);
                if (it->pos > cursor) slot.clips.push_back(AafFiller{it->pos - cursor});
                AafSourceClip sc;
                sc.start = clip->essence_offset;
                sc.length = it->len;
                const aafiAudioEssenceFile* f =
                    clip->Essence ? clip->Essence : nullptr;
                const std::string file = f && f->original_file_path ? f->original_file_path : "";
                auto found = sources.find(file);
                if (found == sources.end()) {
                    AafMob m;
                    m.is_source = true;
                    m.name = f && f->name ? f->name : "";
                    m.mob_id = file;
                    m.media_path = f && f->usable_file_path ? f->usable_file_path : file;
                    found = sources.emplace(file, media.size()).first;
                    media.push_back(std::move(m));
                }
                sc.mob_id = media[found->second].mob_id;
                slot.clips.push_back(sc);
                cursor = it->pos + it->len;
            }
            comp.slots.push_back(std::move(slot));
        }
        out.mobs.push_back(std::move(comp));
        for (AafMob& m : media) out.mobs.push_back(std::move(m));
    }
};

}  // namespace

AafReader* make_libaaf_reader() { return new LibAafReader; }

}  // namespace sonora::interchange
