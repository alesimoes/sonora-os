    // Quality changes the oversampling factor, and so the latency. Silence
    // leaves the tape's state at zero, so the preroll is free.
    {"tape", [] { return native::make_tape(); }, native::suites::tape_golden_config, true,
     {"drive", 18.0f}, {{"quality", 2.0f}}},
