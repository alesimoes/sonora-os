    // Quality changes the oversampling factor, and so the latency. Silence
    // leaves every state at zero, so the preroll is free.
    {"saturator", [] { return native::make_saturator(); }, native::suites::saturator_golden_config, true,
     {"drive", 30.0f}, {{"quality", 2.0f}}},
