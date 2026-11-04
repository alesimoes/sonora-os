    // Zero latency, no response loaded: a unit impulse under the output and
    // mix controls, which is what the host can reach (see golden_file).
    {"cab", [] { return native::make_cab(); }, native::suites::cab_golden_config, true,
     {"output", -9.0f}, {}, false},
