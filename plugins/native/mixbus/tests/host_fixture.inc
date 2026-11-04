    // Quality changes the oversampling factor, and so the latency. Silence
    // leaves the detectors, the side-chain filter and the oversampler at zero;
    // the transformer at rest is an equilibrium (checked by the fixture).
    {"mixbus", [] { return native::make_mixbus(); }, native::suites::mixbus_golden_config, true,
     {"threshold", -32.0f}, {{"quality", 0.0f}}},
