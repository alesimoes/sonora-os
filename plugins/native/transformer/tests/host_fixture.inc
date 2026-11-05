    // The core is a reconfigure parameter with no latency change; silence
    // leaves the core demagnetised, so the preroll is free.
    {"transformer", [] { return native::make_transformer(); }, native::suites::transformer_golden_config, true,
     {"drive", 18.0f}, {{"core", 2.0f}}},
