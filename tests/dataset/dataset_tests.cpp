#include "signal_studio/dataset/dataset.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void check(bool cond, std::string_view msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++g_failures;
  }
}

int case_append_query_commit() {
  using namespace signal::dataset;
  const auto tmp = std::filesystem::temp_directory_path() / "signal_dataset_test.json";
  std::filesystem::remove(tmp);
  JsonFileDataset ds(tmp);
  check(ds.size() == 0, "empty initially");
  SampleRecord r1;
  r1.sample_id = "s1";
  r1.label = "tone";
  r1.source_format = "wav";
  r1.sample_count = 1024;
  r1.sample_rate_hz = 64000.0;
  check(ds.append(r1).ok(), "append s1");
  SampleRecord r2;
  r2.sample_id = "s2";
  r2.label = "noise";
  r2.source_format = "sc16";
  r2.sample_count = 2048;
  r2.sample_rate_hz = 50.0e6;
  check(ds.append(r2).ok(), "append s2");
  check(ds.size() == 2, "size 2");
  auto tones = ds.query({std::string("tone"), {}, {}});
  check(tones.size() == 1 && tones[0].sample_id == "s1", "query by label");
  auto sc16 = ds.query({{}, std::string("sc16"), {}});
  check(sc16.size() == 1 && sc16[0].sample_id == "s2", "query by format");
  auto limited = ds.query({{}, {}, std::uint64_t{1}});
  check(limited.size() == 1, "query with limit");
  check(ds.commit().ok(), "commit");
  check(std::filesystem::exists(tmp), "manifest written");
  std::filesystem::remove(tmp);
  return g_failures == 0 ? 0 : 1;
}

int case_roundtrip() {
  using namespace signal::dataset;
  const auto tmp = std::filesystem::temp_directory_path() / "signal_dataset_rt.json";
  std::filesystem::remove(tmp);
  {
    JsonFileDataset ds(tmp);
    SampleRecord r;
    r.sample_id = "rt1";
    r.data_path = std::filesystem::path("data/sc16/rt1.sc16");
    r.label = "chirp";
    r.source_format = "sc16";
    r.sample_count = 4096;
    r.sample_rate_hz = 12.8e6;
    r.sha256_digest = "abc123";
    check(ds.append(r).ok(), "append rt1");
    check(ds.commit().ok(), "commit rt1");
  }
  // Reopen and verify persistence.
  JsonFileDataset ds(tmp);
  check(ds.size() == 1, "reloaded size 1");
  auto records = ds.records();
  check(records.size() == 1 && records[0].sample_id == "rt1", "rt1 persisted");
  check(records[0].label == "chirp", "label persisted");
  check(records[0].sha256_digest == "abc123", "sha256 persisted");
  check(records[0].sample_count == 4096, "sample_count persisted");
  std::filesystem::remove(tmp);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view{argv[1]} != "--case") {
    std::cerr << "usage: dataset_tests --case <name>\n";
    return 2;
  }
  std::string_view name = argv[2];
  if (name == "append-query-commit") return case_append_query_commit();
  if (name == "roundtrip") return case_roundtrip();
  std::cerr << "unknown case: " << name << "\n";
  return 2;
}
