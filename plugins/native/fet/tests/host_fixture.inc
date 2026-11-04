    // Quality changes the oversampling factor, and so the latency. The
    // rest point is an equilibrium the first steps settle to within
    // 1e-11 V, far inside the golden bar, so the preroll is free.
    {"fet", [] { return native::make_fet(); }, native::suites::fet_golden_config, true,
     {"ratio", 3.0f}, {{"quality", 0.0f}}},
