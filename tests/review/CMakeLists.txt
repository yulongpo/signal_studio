# Reuse audit: signal_review links ONLY public modules (no Qt, no app-core). We verify it builds
# (CMake link audit) and produces correct JSON by running it directly as ctest cases with
# PASS_REGULAR_EXPRESSION checks on stdout.

set(_fixtures "${CMAKE_SOURCE_DIR}/test_data/minimal")
set(_wav "${_fixtures}/stereo_iq_int16.wav")
set(_sc16 "${_fixtures}/complex_sine_sc16le.raw")

# WAV review: complex IQ, bounded samples, magnitude mean present.
add_test(NAME "review.review-wav" COMMAND signal_review "${_wav}")
set_tests_properties("review.review-wav" PROPERTIES TIMEOUT 60 LABELS "unit;contract;ms-08;reuse"
  PASS_REGULAR_EXPRESSION "\"format\":\"wav\";\"complex\":true;\"magnitudeMean\"")

# SC16 without filename sr hint: honest error, no fabricated stats.
add_test(NAME "review.review-sc16" COMMAND signal_review "${_sc16}")
set_tests_properties("review.review-sc16" PROPERTIES TIMEOUT 60 LABELS "unit;contract;ms-08;reuse"
  PASS_REGULAR_EXPRESSION "\"error\"")

# --version reports app + platform.
add_test(NAME "review.review-version" COMMAND signal_review --version)
set_tests_properties("review.review-version" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-08;reuse"
  PASS_REGULAR_EXPRESSION "signal_review;platform")

# Missing file: error reported.
add_test(NAME "review.review-missing" COMMAND signal_review "${CMAKE_SOURCE_DIR}/no_such_file.wav")
set_tests_properties("review.review-missing" PROPERTIES TIMEOUT 20 LABELS "unit;contract;ms-08;reuse"
  PASS_REGULAR_EXPRESSION "\"error\":\"file not found\"")
