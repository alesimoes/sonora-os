    // Iron is a reconfigure parameter with no latency change, and the
    // golden config leaves it on so the golden file covers the core too.
    {"passiveeq", [] { return native::make_passiveeq(); }, native::suites::passiveeq_golden_config, true,
     {"lf_boost", 6.0f}, {{"iron", 1.0f}}},
