    // Quality changes the oversampling factor, and so the latency.
    {"varimu", [] { return native::make_varimu(); }, native::suites::varimu_golden_config, true,
     {"threshold", -40.0f}, {{"quality", 1.0f}}},
