    // Range changes the latency. The shifter's grain schedule runs from the
    // first sample, so a preroll of silence moves it: the reference after the
    // host's preroll is a different render of the same signal, not the
    // golden file's.
    {"tuner", [] { return native::make_tuner(); }, native::suites::tuner_golden_config, false,
     {"amount", 40.0f}, {{"range", 2.0f}}},
