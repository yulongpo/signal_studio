#include "signal_studio/visualization/chart_view.hpp"
#include "signal_studio/visualization/data_series.hpp"
#include "signal_studio/visualization/overlay.hpp"
#include "signal_studio/visualization/views.hpp"
#include "signal_studio/visualization/viewport.hpp"

#include <QApplication>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;
QApplication* g_app = nullptr;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}
bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void ensure_app() {
  if (!g_app) {
    static int argc = 1;
    static char arg0[] = "visualization_tests";
    static char* argv[] = {arg0, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    g_app = new QApplication(argc, argv);
  }
}

int case_viewport_window_clamp() {
  signal::visualization::ViewportRange r(0.0, 10.0);
  check(r.set_window(2.0, 5.0).ok(), "set_window ok");
  check(approx(r.window_lo(), 2.0, 1e-9) && approx(r.window_hi(), 5.0, 1e-9), "window set");
  check(!r.set_window(5.0, 2.0).ok(), "inverted window rejected");
  check(r.set_window(-5.0, 100.0).ok(), "out-of-range clamped");
  check(approx(r.window_lo(), 0.0, 1e-9) && approx(r.window_hi(), 10.0, 1e-9), "clamped to extent");
  return g_failures == 0 ? 0 : 1;
}

int case_viewport_zoom_pan() {
  signal::visualization::ViewportRange r(0.0, 10.0);
  check(r.set_window(0.0, 10.0).ok(), "full window");
  check(r.zoom(5.0, 0.5).ok(), "zoom in ok");
  check(approx(r.window_hi() - r.window_lo(), 5.0, 1e-9), "zoom halved window span");
  check(r.pan(0.5).ok(), "pan ok");
  // After zoom to span 5 centered at 5 (window 2.5..7.5), pan +0.5*5 = +2.5 -> 5..10.
  check(approx(r.window_lo(), 5.0, 1e-9) && approx(r.window_hi(), 10.0, 1e-9), "pan then clamp");
  r.reset();
  check(approx(r.window_hi() - r.window_lo(), 10.0, 1e-9), "reset restores full window");
  return g_failures == 0 ? 0 : 1;
}

int case_frequency_unit() {
  using namespace signal::visualization;
  check(frequency_unit_for(999.0) == "Hz", "999 Hz");
  check(frequency_unit_for(1000.0) == "kHz", "1000 Hz -> kHz");
  check(frequency_unit_for(1.5e6) == "MHz", "1.5 MHz");
  check(frequency_unit_for(2.4e9) == "GHz", "2.4 GHz");
  return g_failures == 0 ? 0 : 1;
}

int case_format_frequency() {
  using namespace signal::visualization;
  std::string s = format_frequency(1234567.0);
  check(s.find("MHz") != std::string::npos, "format MHz unit");
  check(format_frequency(0.0).find("Hz") != std::string::npos, "format 0 Hz");
  return g_failures == 0 ? 0 : 1;
}

int case_overlay_model() {
  using namespace signal::visualization;
  OverlayModel m;
  m.add_cursor({1.5e6, "marker1"});
  m.add_cursor({2.5e6, "marker2"});
  check(m.cursors().size() == 2, "two cursors");
  check(m.set_frequency_selection(1.0e6, 2.0e6).ok(), "selection ok");
  check(m.selection().has_value() && approx(m.selection()->x_lo, 1.0e6, 1.0), "selection lo");
  check(!m.set_frequency_selection(2.0e6, 1.0e6).ok(), "inverted selection rejected");
  m.set_measurement({"delta", 1.0e6, "Hz"});
  check(m.measurement().has_value() && m.measurement()->label == "delta", "measurement set");
  m.clear_selection();
  check(!m.selection().has_value(), "selection cleared");
  return g_failures == 0 ? 0 : 1;
}

int case_data_series() {
  using namespace signal::visualization;
  RealSeries rs("real", {1.0, 2.0, 3.0, 4.0}, 1000.0);
  check(rs.kind() == SeriesKind::time_waveform, "real kind");
  check(rs.point_count() == 4, "real count");
  check(approx(rs.sample_rate_hz(), 1000.0, 1e-9), "real sr");
  SpectrumSeries ss("spec", {0.0, 1.0, 2.0}, {-10.0, -5.0, -20.0}, 1000.0);
  check(ss.point_count() == 3 && ss.power_db().size() == 3, "spectrum count");
  SpectrogramSeries sg("sg", {0.0, 1.0}, {0.0, 1.0}, {1.0, 2.0, 3.0, 4.0}, 2, 2);
  check(sg.frame_count() == 2 && sg.freq_count() == 2, "spectrogram dims");
  ComplexSeries cs("iq", {1.0, 0.0}, {0.0, 1.0}, 1000.0);
  check(cs.point_count() == 2, "complex count");
  return g_failures == 0 ? 0 : 1;
}

int case_color_map() {
  using namespace signal::visualization;
  auto gray0 = color_rgb(ColorScale::grayscale, 0.0);
  auto gray1 = color_rgb(ColorScale::grayscale, 1.0);
  check((gray0 & 0xFFFFFF) == 0x000000, "grayscale 0 black");
  check((gray1 & 0xFFFFFF) == 0xFFFFFF, "grayscale 1 white");
  auto v0 = color_rgb(ColorScale::viridis, 0.0);
  auto v1 = color_rgb(ColorScale::viridis, 1.0);
  check(v0 != v1, "viridis endpoints differ");
  return g_failures == 0 ? 0 : 1;
}

int case_view_factory() {
  using namespace signal::visualization;
  ensure_app();
  auto tw = make_time_waveform_view();
  auto sp = make_spectrum_view();
  auto sg = make_spectrogram_view();
  auto co = make_constellation_view();
  auto ey = make_eye_diagram_view();
  check(tw.ok() && sp.ok() && sg.ok() && co.ok() && ey.ok(), "all view factories succeed");
  check((*tw)->kind() == ChartViewKind::time_waveform, "time waveform kind");
  check((*sg)->kind() == ChartViewKind::spectrogram, "spectrogram kind");
  // Bind and verify native widget non-null.
  auto series = std::make_shared<RealSeries>("real", std::vector<double>(100, 0.0), 1000.0);
  (*tw)->bind(series);
  check((*tw)->bound_series() != nullptr, "bound series retained");
  check((*tw)->native_widget() != nullptr, "native widget non-null");
  // Hide-and-stop: hidden view reports no data need.
  (*tw)->set_visible(true);
  check((*tw)->needs_data(), "visible view needs data");
  (*tw)->set_visible(false);
  check(!(*tw)->needs_data(), "hidden view stops data need");
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: visualization_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "viewport-window-clamp") return case_viewport_window_clamp();
  if (name == "viewport-zoom-pan") return case_viewport_zoom_pan();
  if (name == "frequency-unit") return case_frequency_unit();
  if (name == "format-frequency") return case_format_frequency();
  if (name == "overlay-model") return case_overlay_model();
  if (name == "data-series") return case_data_series();
  if (name == "color-map") return case_color_map();
  if (name == "view-factory") return case_view_factory();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
