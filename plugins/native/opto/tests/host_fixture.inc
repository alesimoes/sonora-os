    // Quality changes the oversampling factor, and so the latency.
    {"opto", [] { return native::make_opto(); }, native::suites::opto_golden_config, true,
     {"peak_reduction", 95.0f}, {{"quality", 0.0f}}},
