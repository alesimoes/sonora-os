    // A 48 kHz capture's latency with nothing loaded: the input, delayed at
    // 44.1 and 96 kHz, under the output and mix controls. The golden files
    // are rendered with a capture loaded; the bundle here has none, so
    // they are not compared (see the cabinet's entry).
    {"amp", [] { return native::make_amp(); }, native::suites::amp_golden_config, true,
     {"output", -9.0f}, {}, false},
