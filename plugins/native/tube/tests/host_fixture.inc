    // Quality changes the oversampling factor, and so the latency.
    {"tube", [] { return native::make_tube(); }, native::suites::tube_golden_config, true,
     {"drive", 24.0f}, {{"quality", 2.0f}}},
