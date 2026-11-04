    // The stages run in the console, and silence leaves every one of them
    // (and the loudness meter) at rest, so the preroll is free. There is no
    // latency-changing parameter.
    {"mastering", [] { return native::make_mastering(); }, native::suites::mastering_golden_config, true,
     {"gain", -6.0f}, {}},
