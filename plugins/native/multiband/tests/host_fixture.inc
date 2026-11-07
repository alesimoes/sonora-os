    // The look-ahead reconfigures, and so changes the reported latency.
    {"multiband", [] { return native::make_multiband(); }, native::suites::multiband_golden_config, true,
     {"b1_threshold", -40.0f}, {{"lookahead", 3.0f}}},
