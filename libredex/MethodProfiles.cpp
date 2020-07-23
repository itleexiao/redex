/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodProfiles.h"

#include <iostream>
#include <stdio.h>
#include <stdlib.h>

#include "Timer.h"
#include "Trace.h"

using namespace method_profiles;

extern int errno;

const StatsMap& MethodProfiles::method_stats(
    const std::string& interaction_id) const {
  const auto& search1 = m_method_stats.find(interaction_id);
  if (search1 != m_method_stats.end()) {
    return search1->second;
  }
  if (interaction_id == COLD_START) {
    // Originally, the stats file had no interaction_id column and it only
    // covered coldstart. Search for the default (empty string) for backwards
    // compatibility when we're searching for coldstart but it's not found.
    const auto& search2 = m_method_stats.find("");
    if (search2 != m_method_stats.end()) {
      return search2->second;
    }
  }

  static StatsMap empty_map = {};
  return empty_map;
}

bool MethodProfiles::parse_stats_file(const std::string& csv_filename) {
  TRACE(METH_PROF, 3, "input csv filename: %s", csv_filename.c_str());
  if (csv_filename.empty()) {
    TRACE(METH_PROF, 2, "No csv file given");
    return false;
  }
  Timer t("Parsing agg_method_stats_file");

  auto cleanup = [](FILE* fp, char* line = nullptr) {
    if (fp) {
      fclose(fp);
    }
    if (line) {
      // getline allocates buffer space and expects us to free it
      free(line);
    }
  };

  // Using C-style file reading and parsing because it's faster than the
  // iostreams equivalent and we expect to read very large csv files.
  FILE* fp = fopen(csv_filename.c_str(), "r");
  if (fp == nullptr) {
    std::cerr << "FAILED to open " << csv_filename << ": " << strerror(errno)
              << "\n";
    cleanup(fp);
    return false;
  }

  // getline will allocate a buffer and put a pointer to it here
  char* line = nullptr;
  size_t len = 0;
  while (getline(&line, &len, fp) != -1) {
    bool success = false;
    if (m_mode == NONE) {
      success = parse_header(line);
    } else {
      success = parse_line(line);
    }
    if (!success) {
      cleanup(fp, line);
      return false;
    }
  }
  if (errno != 0) {
    std::cerr << "FAILED to read a line: " << strerror(errno) << "\n";
    cleanup(fp, line);
    return false;
  }

  size_t total_rows = 0;
  for (const auto& pair : m_method_stats) {
    total_rows += pair.second.size();
  }
  TRACE(METH_PROF, 1,
        "MethodProfiles successfully parsed %zu rows; %zu unresolved lines",
        total_rows, unresolved_size());
  cleanup(fp, line);
  return true;
}

int64_t parse_int(const char* tok) {
  char* rest = nullptr;
  const auto result = strtol(tok, &rest, 10);
  const auto len = strlen(rest);
  always_assert_log(rest != tok, "can't parse %s into a int64_t", tok);
  always_assert_log(len == 0 || (len == 1 && rest[0] == '\n'),
                    "can't parse %s into a int64_t", tok);
  return result;
}

double parse_double(const char* tok) {
  char* rest = nullptr;
  const auto result = strtod(tok, &rest);
  const auto len = strlen(rest);
  always_assert_log(rest != tok, "can't parse %s into a double", tok);
  always_assert_log(len == 0 || (len == 1 && rest[0] == '\n'),
                    "can't parse %s into a double", tok);
  return result;
}

bool MethodProfiles::parse_metadata(char* line) {
  always_assert(m_mode == METADATA);
  uint32_t interaction_count{0};
  auto parse_cell = [&](char* tok, uint32_t i) -> bool {
    switch (i) {
    case 0:
      m_interaction_id = std::string(tok);
      return true;
    case 1: {
      int64_t parsed = parse_int(tok);
      always_assert(parsed <= std::numeric_limits<uint32_t>::max());
      always_assert(parsed >= 0);
      interaction_count = static_cast<uint32_t>(parsed);
      return true;
    }
    default:
      return false;
    }
  };
  bool success = parse_cells(line, parse_cell);
  if (!success) {
    return false;
  }
  m_interaction_counts.emplace(m_interaction_id, interaction_count);
  // There should only be one line of metadata per file. Once we've processed
  // it, change the parsing mode back to NONE.
  m_mode = NONE;
  return true;
}

bool MethodProfiles::parse_main(char* line) {
  always_assert(m_mode == MAIN);
  Stats stats;
  std::string interaction_id;
  DexMethodRef* ref = nullptr;
  auto parse_cell = [&](char* tok, uint32_t i) -> bool {
    switch (i) {
    case INDEX:
      // Don't need this raw data. It's an arbitrary index (the line number in
      // the file)
      return true;
    case NAME:
      ref = DexMethod::get_method(tok);
      if (ref == nullptr) {
        TRACE(METH_PROF, 6, "failed to resolve %s", tok);
      }
      return true;
    case APPEAR100:
      stats.appear_percent = parse_double(tok);
      return true;
    case APPEAR_NUMBER:
      // Don't need this raw data. appear_percent is the same thing but
      // normalized
      return true;
    case AVG_CALL:
      stats.call_count = parse_double(tok);
      return true;
    case AVG_ORDER:
      // Don't need this raw data. order_percent is the same thing but
      // normalized
      return true;
    case AVG_RANK100:
      stats.order_percent = parse_double(tok);
      return true;
    case MIN_API_LEVEL: {
      int64_t level = parse_int(tok);
      always_assert(level <= std::numeric_limits<int16_t>::max());
      always_assert(level >= std::numeric_limits<int16_t>::min());
      stats.min_api_level = static_cast<int16_t>(level);
      return true;
    }
    default:
      const auto& search = m_optional_columns.find(i);
      if (search != m_optional_columns.end()) {
        if (search->second == "interaction") {
          interaction_id = tok;
          if (interaction_id.back() == '\n') {
            interaction_id.resize(interaction_id.size() - 1);
          }
          return true;
        }
      }
      std::cerr << "FAILED to parse line. Unknown extra column\n";
      return false;
    }
  };

  std::string copy(line);
  bool success = parse_cells(line, parse_cell);
  if (!success) {
    return false;
  }
  if (interaction_id.empty()) {
    // Interaction IDs from the current row have priority over the interaction
    // id from the top of the file. This shouldn't happen in practice, but this
    // is the conservative approach.
    interaction_id = m_interaction_id;
  }
  if (ref != nullptr) {
    TRACE(METH_PROF, 6, "(%s, %s) -> {%f, %f, %f, %d}", SHOW(ref),
          interaction_id.c_str(), stats.appear_percent, stats.call_count,
          stats.order_percent, stats.min_api_level);
    m_method_stats[interaction_id].emplace(ref, stats);
  } else {
    m_unresolved_lines[interaction_id].push_back(copy);
    TRACE(METH_PROF, 6, "unresolved: %s", copy.c_str());
  }
  return true;
}

bool MethodProfiles::parse_line(char* line) {
  if (m_mode == MAIN) {
    return parse_main(line);
  } else if (m_mode == METADATA) {
    return parse_metadata(line);
  } else {
    always_assert_log(false, "invalid parsing mode");
  }
}

boost::optional<uint32_t> MethodProfiles::get_interaction_count(
    const std::string& interaction_id) {
  const auto& search = m_interaction_counts.find(interaction_id);
  if (search == m_interaction_counts.end()) {
    return boost::none;
  } else {
    return search->second;
  }
}

void MethodProfiles::process_unresolved_lines() {
  auto unresolved_lines = m_unresolved_lines;
  m_unresolved_lines.clear();
  for (auto& pair : unresolved_lines) {
    m_interaction_id = pair.first;
    for (auto& line : pair.second) {
      bool success = parse_main(const_cast<char*>(line.c_str()));
      always_assert(success);
    }
  }

  size_t total_rows = 0;
  for (const auto& pair : m_method_stats) {
    total_rows += pair.second.size();
  }
  TRACE(METH_PROF, 1,
        "After processing unresolved lines: MethodProfiles successfully parsed "
        "%zu rows; %zu unresolved lines",
        total_rows, unresolved_size());
}

bool MethodProfiles::parse_header(char* line) {
  always_assert(m_mode == NONE);
  auto check_cell = [](const char* expected, const char* tok,
                       uint32_t i) -> bool {
    const size_t MAX_CELL_LENGTH = 1000;
    if (strncmp(tok, expected, MAX_CELL_LENGTH) != 0) {
      std::cerr << "Unexpected Header (column " << i << "): " << tok
                << " != " << expected << "\n";
      return false;
    }
    return true;
  };
  if (strncmp(line, "interaction", 11) == 0) {
    m_mode = METADATA;
    // Extra metadata at the top of the file that we want to parse
    auto parse_cell = [&](char* tok, uint32_t i) -> bool {
      switch (i) {
      case 0:
        return check_cell("interaction", tok, i);
      case 1:
        return check_cell("appear#", tok, i);
      default: {
        auto len = strnlen(tok, 100);
        bool ok = (len == 0 || (len == 1 && tok[0] == '\n'));
        if (!ok) {
          std::cerr << "Unexpected Metadata Column: " << tok << std::endl;
        }
        return ok;
      }
      }
    };
    return parse_cells(line, parse_cell);
  } else {
    m_mode = MAIN;
    auto parse_cell = [&](char* tok, uint32_t i) -> bool {
      switch (i) {
      case INDEX:
        return check_cell("index", tok, i);
      case NAME:
        return check_cell("name", tok, i);
      case APPEAR100:
        return check_cell("appear100", tok, i);
      case APPEAR_NUMBER:
        return check_cell("appear#", tok, i);
      case AVG_CALL:
        return check_cell("avg_call", tok, i);
      case AVG_ORDER:
        return check_cell("avg_order", tok, i);
      case AVG_RANK100:
        return check_cell("avg_rank100", tok, i);
      case MIN_API_LEVEL:
        return check_cell("min_api_level", tok, i);
      default:
        std::string column_name = tok;
        if (column_name.back() == '\n') {
          column_name.resize(column_name.size() - 1);
        }
        m_optional_columns.emplace(i, column_name);
        return true;
      }
    };
    return parse_cells(line, parse_cell);
  }
}

dexmethods_profiled_comparator::dexmethods_profiled_comparator(
    const method_profiles::MethodProfiles* method_profiles,
    const std::unordered_set<std::string>* whitelisted_substrings,
    std::unordered_map<DexMethod*, double>* cache,
    bool legacy_order)
    : m_method_profiles(method_profiles),
      m_whitelisted_substrings(whitelisted_substrings),
      m_cache(cache),
      m_legacy_order(legacy_order) {
  always_assert(m_method_profiles != nullptr);
  always_assert(m_whitelisted_substrings != nullptr);
  always_assert(m_cache != nullptr);

  m_coldstart_start_marker = static_cast<DexMethod*>(
      DexMethod::get_method("Lcom/facebook/common/methodpreloader/primarydeps/"
                            "StartColdStartMethodPreloaderMethodMarker;"
                            ".startColdStartMethods:()V"));
  m_coldstart_end_marker = static_cast<DexMethod*>(
      DexMethod::get_method("Lcom/facebook/common/methodpreloader/primarydeps/"
                            "EndColdStartMethodPreloaderMethodMarker;"
                            ".endColdStartMethods:()V"));

  for (const auto& pair : m_method_profiles->all_interactions()) {
    std::string interaction_id = pair.first;
    if (interaction_id.empty()) {
      // For backwards compatibility. Older versions of the aggregate profiles
      // only have cold start (and no interaction_id column)
      interaction_id = COLD_START;
    }
    if (!m_legacy_order || interaction_id == COLD_START) {
      m_interactions.push_back(interaction_id);
    }
  }
  std::sort(m_interactions.begin(), m_interactions.end(),
            [](const std::string& a, const std::string& b) {
              if (a == COLD_START && b != COLD_START) {
                // Cold Start always comes first;
                return true;
              }
              // TODO: use interaction prevalence
              return a < b;
            });
}

double dexmethods_profiled_comparator::get_method_sort_num(
    const DexMethod* method) {
  double range_begin = 0.0;
  for (const auto& interaction_id : m_interactions) {
    if (interaction_id == COLD_START && m_coldstart_start_marker != nullptr &&
        m_coldstart_end_marker != nullptr) {
      if (method == m_coldstart_start_marker) {
        return range_begin;
      } else if (method == m_coldstart_end_marker) {
        return range_begin + RANGE_SIZE;
      }
    }
    const auto& stats_map = m_method_profiles->method_stats(interaction_id);
    auto it = stats_map.find(method);
    if (it != stats_map.end()) {
      const auto& stat = it->second;
      if (m_legacy_order && stat.appear_percent >= 95.0) {
        return range_begin + RANGE_SIZE / 2;
      } else if (!m_legacy_order && stat.appear_percent >= 90.0) {
        // TODO iterate on this ordering heuristic
        return range_begin + stat.order_percent * RANGE_SIZE / 100.0;
      }
    }
    range_begin += RANGE_STRIDE;
  }

  // If the method is not present in profiled order file we'll put it in the
  // end of the code section
  return VERY_END;
}

double dexmethods_profiled_comparator::get_method_sort_num_override(
    const DexMethod* method) {
  const std::string& deobfname = method->get_deobfuscated_name();
  for (const std::string& substr : *m_whitelisted_substrings) {
    if (deobfname.find(substr) != std::string::npos) {
      return COLD_START_RANGE_BEGIN + RANGE_SIZE / 2;
    }
  }
  return VERY_END;
}

bool dexmethods_profiled_comparator::operator()(DexMethod* a, DexMethod* b) {
  if (a == nullptr) {
    return b != nullptr;
  } else if (b == nullptr) {
    return false;
  }

  auto get_sort_num = [this](DexMethod* m) -> double {
    const auto& search = m_cache->find(m);
    if (search != m_cache->end()) {
      return search->second;
    }

    double w = get_method_sort_num(m);
    if (w == VERY_END) {
      // For methods not included in the profiled methods file, move them to
      // the top section anyway if they match one of the whitelisted
      // substrings.
      w = get_method_sort_num_override(m);
    }

    m_cache->emplace(m, w);
    return w;
  };

  double sort_num_a = get_sort_num(a);
  double sort_num_b = get_sort_num(b);

  if (sort_num_a == sort_num_b) {
    return compare_dexmethods(a, b);
  }

  return sort_num_a < sort_num_b;
}
