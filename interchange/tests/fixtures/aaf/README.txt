CC0 fixtures for the AAF import path (ADR-0154).

minimal.aaf is NOT committed yet. A real AAF is a structured-storage
container that only libaaf-class tooling or a DAW can write, and neither is
available in the build environment; a hand-made binary would be unverifiable.
Author it with a DAW export (one stereo track, one clip, one fade), release it
CC0, and drop it here: tests/test_aaf_fixture.cpp then runs it on its own.
Until then that test reports SKIPPED (exit 77), visibly, in ctest.
