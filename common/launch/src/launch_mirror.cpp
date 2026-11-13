#include "sonora/launch/launch_mirror.h"

namespace sonora::launch {

void LaunchMirror::assign(std::uint32_t track, std::uint32_t scene, ClipId clip) {
    if (grid_.contains(track, scene)) grid_.cell(track, scene).clip = clip;
}

void LaunchMirror::apply(const Transition& tr) {
    if (!grid_.contains(tr.track, tr.scene)) return;
    Cell& cell = grid_.cell(tr.track, tr.scene);
    switch (tr.kind) {
    case Transition::Kind::Start:
        cell.state = CellState::Playing;
        break;
    case Transition::Kind::Stop:
        cell.state = CellState::Stopped;
        break;
    case Transition::Kind::RecordStart:
        cell.state = CellState::Recording;
        break;
    case Transition::Kind::RecordStop:
        // The capture became the cell's clip on the audio side too.
        cell.clip = tr.clip;
        cell.state = CellState::Stopped;
        break;
    }
}

}  // namespace sonora::launch
