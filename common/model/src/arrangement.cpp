// SPDX-License-Identifier: GPL-3.0-or-later
//
// plan_section_edit (ADR-0116). The member count is worked out from the
// snapshot before anything is built, so a section over the cap is refused
// without applying hundreds of members to find out; the plan itself is then
// built by applying each member to a working copy, so positions after a
// split or a shift are read, never recomputed by hand.
#include "sonora/model/arrangement.h"

#include <algorithm>
#include <utility>

namespace sonora::model {

const char* const kSectionEditLimits =
    "automation, tempo and meter changes and crossfades across the cut stay where they were";

std::vector<Marker> section_markers(const Snapshot& s) {
    std::vector<Marker> out;
    if (!s.edit || !s.edit->markers) return out;
    for (const Marker& m : s.edit->markers->markers) {
        if (m.length > 0) out.push_back(m);
    }
    return out;
}

namespace {

const Playlist* active_playlist(const TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

std::vector<const Playlist*> audio_playlists(const Snapshot& s) {
    std::vector<const Playlist*> out;
    for (const auto& t : s.edit->tracks) {
        if (t->kind != TrackKind::Audio) continue;
        if (const Playlist* p = active_playlist(*t)) out.push_back(p);
    }
    return out;
}

const Marker* find_marker(const Snapshot& s, Id id) {
    for (const Marker& m : s.edit->markers->markers) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

class SectionPlanner {
public:
    SectionPlanner(const Snapshot& s, const SectionEdit& e, const Marker& section, Id next_id)
        : work_(s),
          section_(section),
          dest_(e.destination),
          move_(e.mode == SectionMode::Move),
          start_(section.position),
          length_(section.length),
          end_(section.position + section.length),
          // Where the section's material is once the gap at the
          // destination has opened: it shifted too if it was at or after.
          opened_start_(start_ + (e.destination <= start_ ? length_ : 0)),
          landing_(move_ && e.destination > end_ ? e.destination - length_ : e.destination),
          next_(next_id) {
        cuts_ = {start_, end_, dest_};
        std::sort(cuts_.begin(), cuts_.end());
        cuts_.erase(std::unique(cuts_.begin(), cuts_.end()), cuts_.end());
    }

    SectionPlan run() {
        SectionPlan plan;
        plan.refusal = check_timeline();
        if (!plan.refusal.empty()) return plan;
        std::size_t content = 0;
        std::size_t count = region_member_count(content);
        plan_markers();
        if (content == 0 && content_markers_ == 0) {
            plan.refusal = "section \"" + section_.name + "\" holds no audio and no markers";
            return plan;
        }
        count += marker_ops_.size();
        if (count > kMaxCompoundMembers) {
            plan.refusal = over_cap(count);
            return plan;
        }
        for (const Playlist* p : audio_playlists(work_)) {
            if (!plan_playlist(p->id)) return failed();
        }
        for (Member& m : marker_ops_) {
            if (AddMarker* add = m.get_if<AddMarker>()) add->marker.id = next_++;
            if (!step(std::move(m))) return failed();
        }
        // The count above is the cap's only check that runs before the
        // work; this one keeps a miscount from shipping an oversize plan.
        if (out_.ops.size() > kMaxCompoundMembers) {
            plan.refusal = over_cap(out_.ops.size());
            return plan;
        }
        out_.label = move_ ? "Move Section" : "Copy Section";
        plan.compound = std::move(out_);
        return plan;
    }

private:
    std::string over_cap(std::size_t count) const {
        return std::string(move_ ? "moving" : "copying") + " section \"" + section_.name +
               "\" takes " + std::to_string(count) + " operations; one undo step holds at most " +
               std::to_string(kMaxCompoundMembers);
    }

    SectionPlan failed() const {
        SectionPlan plan;
        plan.refusal = "the section edit cannot be planned: " + why_;
        return plan;
    }

    std::string check_timeline() const {
        const std::string past = "the section would push material past the end of the timeline";
        if (dest_ > kMaxInteger - length_) return past;
        for (const Playlist* p : audio_playlists(work_)) {
            for (const Region& r : p->regions) {
                if (r.end() > dest_ && r.end() > kMaxInteger - length_) return past;
            }
        }
        for (const Marker& m : work_.edit->markers->markers) {
            if (m.position >= dest_ && m.position + m.length > kMaxInteger - length_) return past;
        }
        return {};
    }

    // Exactly the region members plan_playlist() emits, from the snapshot
    // alone. A region straddling a cut is split there and its right part
    // straddles no earlier cut, so each straddled cut is one split.
    std::size_t region_member_count(std::size_t& content) const {
        std::size_t count = 0;
        for (const Playlist* p : audio_playlists(work_)) {
            std::size_t inside = 0;
            bool after_dest = false;
            bool after_end = false;
            for (const Region& r : p->regions) {
                for (const Frames c : cuts_) {
                    if (r.position < c && c < r.end()) ++count;
                }
                if (r.position < end_ && r.end() > start_) ++inside;
                after_dest = after_dest || r.end() > dest_;
                after_end = after_end || r.end() > end_;
            }
            count += inside + (after_dest ? 1 : 0);
            if (move_ && (after_end || (dest_ > end_ && inside > 0))) ++count;
            content += inside;
        }
        return count;
    }

    // Where a position outside the section ends up.
    Frames moved(Frames x) const {
        Frames y = x >= dest_ ? x + length_ : x;
        if (move_ && y >= opened_start_ + length_) y -= length_;
        return y;
    }

    bool inside(const Marker& m) const { return m.position >= start_ && m.position < end_; }

    void plan_markers() {
        for (const Marker& m : work_.edit->markers->markers) {
            const bool own = m.id == section_.id;
            if (inside(m) && !own) ++content_markers_;
            if (!own && !inside(m)) {
                move_marker(m, moved(m.position));
                continue;
            }
            const Frames there = landing_ + (m.position - start_);
            if (move_) {
                move_marker(m, there);
                continue;
            }
            move_marker(m, moved(m.position));
            Marker copy = m;
            copy.id = 0;  // taken when the regions have theirs
            copy.position = there;
            marker_ops_.emplace_back(AddMarker{copy});
        }
    }

    void move_marker(const Marker& m, Frames to) {
        if (to != m.position) marker_ops_.emplace_back(MoveMarker{m.id, to});
    }

    bool plan_playlist(Id playlist) {
        for (const Frames c : cuts_) {
            if (const Region* r = straddling(playlist, c)) {
                const Id right = next_++;
                if (!step(SplitRegion{playlist, r->id, c, right, Timebase::Frames})) return false;
            }
        }
        std::vector<Id> content;
        for (const Region& r : find_playlist(playlist)->regions) {
            if (r.position >= start_ && r.end() <= end_) content.push_back(r.id);
        }
        if (starts_from(playlist, dest_) && !step(ShiftRegions{playlist, dest_, length_})) {
            return false;
        }
        for (const Id id : content) {
            const Region& r = *find_region(playlist, id);
            const Frames there = dest_ + (r.position - opened_start_);
            if (move_) {
                if (!step(MoveRegion{playlist, id, there, 0, Timebase::Frames})) return false;
                continue;
            }
            Region copy = r;
            copy.id = next_++;
            copy.position = there;
            // A copy joins none of the originals' clip groups; a loop
            // repeat outside a group is invalid, so it is a plain region.
            copy.clip_group = 0;
            copy.loop = 0;
            // The gap is empty, so the copy needs no split; 0 makes apply()
            // refuse instead of overwriting should that ever be wrong.
            if (!step(PlaceRegion{playlist, std::move(copy), 0})) return false;
        }
        const Frames hole_end = opened_start_ + length_;
        if (move_ && starts_from(playlist, hole_end) &&
            !step(ShiftRegions{playlist, hole_end, -length_})) {
            return false;
        }
        return true;
    }

    const Playlist* find_playlist(Id id) const {
        for (const Playlist* p : audio_playlists(work_)) {
            if (p->id == id) return p;
        }
        return nullptr;
    }

    const Region* find_region(Id playlist, Id id) const {
        for (const Region& r : find_playlist(playlist)->regions) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    const Region* straddling(Id playlist, Frames at) const {
        for (const Region& r : find_playlist(playlist)->regions) {
            if (r.position < at && at < r.end()) return &r;
        }
        return nullptr;
    }

    bool starts_from(Id playlist, Frames from) const {
        for (const Region& r : find_playlist(playlist)->regions) {
            if (r.position >= from) return true;
        }
        return false;
    }

    bool step(Member m) {
        Snapshot next;
        std::string why;
        if (!apply(work_, to_operation(m), next, why)) {
            why_ = why;
            return false;
        }
        work_ = std::move(next);
        out_.ops.push_back(std::move(m));
        return true;
    }

    Snapshot work_;
    const Marker section_;
    const Frames dest_;
    const bool move_;
    const Frames start_;
    const Frames length_;
    const Frames end_;
    const Frames opened_start_;
    const Frames landing_;
    Id next_;
    std::vector<Frames> cuts_;
    std::vector<Member> marker_ops_;
    std::size_t content_markers_ = 0;
    Compound out_;
    std::string why_;
};

}  // namespace

SectionPlan plan_section_edit(const Snapshot& s, const SectionEdit& edit, Id next_id) {
    SectionPlan plan;
    const Marker* section = s.edit && s.edit->markers ? find_marker(s, edit.section) : nullptr;
    if (section == nullptr) {
        plan.refusal = "marker " + std::to_string(edit.section) + " does not exist";
        return plan;
    }
    if (section->length <= 0) {
        plan.refusal = "marker \"" + section->name + "\" is a point, not a section";
        return plan;
    }
    const Frames start = section->position;
    const Frames end = start + section->length;
    if (edit.destination < 0) {
        plan.refusal = "the destination is before the start of the timeline";
        return plan;
    }
    if (edit.destination > start && edit.destination < end) {
        plan.refusal = "the destination is inside section \"" + section->name + "\" itself";
        return plan;
    }
    if (edit.mode == SectionMode::Move && (edit.destination == start || edit.destination == end)) {
        plan.refusal = "section \"" + section->name + "\" is already there";
        return plan;
    }
    return SectionPlanner(s, edit, *section, next_id).run();
}

}  // namespace sonora::model
