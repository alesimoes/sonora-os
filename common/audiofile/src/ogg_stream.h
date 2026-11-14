// SPDX-License-Identifier: GPL-3.0-or-later
// Ogg pages to a LossyByteSink, shared by the Opus and Vorbis encoders.
#pragma once

#include <ogg/ogg.h>

#include <string>

#include "sonora/lossy_encoder.h"

namespace sonora {

class OggOut {
public:
    explicit OggOut(LossyByteSink& sink) : sink_(sink) {}
    ~OggOut() {
        if (open_) ogg_stream_clear(&os_);
    }
    OggOut(const OggOut&) = delete;
    OggOut& operator=(const OggOut&) = delete;

    void init() {
        // A fixed serial: the stream is alone in its file, and the same
        // render then gives the same bytes.
        ogg_stream_init(&os_, 0x534f4e4f);
        open_ = true;
    }
    ogg_stream_state* state() { return &os_; }

    // `force`: flush the open page too (header packets must end a page).
    bool pages(bool force, std::string& error) {
        ogg_page og;
        for (;;) {
            const int got = force ? ogg_stream_flush(&os_, &og) : ogg_stream_pageout(&os_, &og);
            if (got == 0) return true;
            if (!sink_.write(og.header, static_cast<std::size_t>(og.header_len), error)) return false;
            if (!sink_.write(og.body, static_cast<std::size_t>(og.body_len), error)) return false;
        }
    }

private:
    LossyByteSink& sink_;
    ogg_stream_state os_{};
    bool open_ = false;
};

}  // namespace sonora
