// BLOCKED_UNTIL_AGENT_C
//
// Baseline 82935ab34ec6126707da152bf46d1f352458ab0c has no
// DetectionCard::startLatchActive(). This file must compile on that baseline,
// so it does not call the missing method and does not invent a production stub.
//
// submit() is private. The public click path is QPushButton objectName
// "runWidebandDetection", connected to DetectionCard::submit. A bare
// DetectionCard(session, [] { return QString{}; }) returns from submit before
// requestSubmitted: sourceValidationError requires a saved project directory
// and a real ComplexIQ file. Do not treat zero emissions from that empty
// fixture as the latch passing.
//
// After Agent C adds startLatchActive(), replace this main with a widget test
// and relink ${SS_UI_SOURCES} like SignalStudioUxTests. Desired R-08, without
// an unfinished run, and without asserting an unmeasured p95:
//   1. Build DetectionCard the way tests/detection_card_ux_tests.h does, on a
//      Session that already has a saved project and a ready ComplexIQ file.
//   2. Count DetectionCard::requestSubmitted.
//   3. QElapsedTimer around two activations of runWidebandDetection.
//   4. Exactly one requestSubmitted.
//   5. Before the first click handler returns, the button is disabled,
//      its text contains 准备检测中, and startLatchActive() is true.
//   6. Print the elapsed milliseconds. Do not require p95 < 100 ms.
//
// On this baseline the second submit may still emit. That desired assertion
// stays out of this translation unit until the latch API exists.

#include <iostream>

int main() {
  std::cout
      << "SKIP ss_de_rpo_ui_tests BLOCKED_UNTIL_AGENT_C "
      << "DetectionCard::startLatchActive is absent on baseline "
      << "82935ab34ec6126707da152bf46d1f352458ab0c. "
      << "Desired R-08: two submits, one requestSubmitted, button disabled, "
      << "text contains 准备检测中. "
      << "QElapsedTimer was not started because the widget test is inactive. "
      << "p95 < 100 ms is not a pass criterion.\n";
  return 0;
}
