#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace onegen::detail {

/** One producer library whose base name is already bound to a different file. */
struct ModuleConflict {
  std::filesystem::path shipped;
  std::filesystem::path loaded;
};

/** One import that no searched location provides. */
struct MissingImport {
  std::string name;
  std::filesystem::path needed_by;
};

/** What loading one CTI in this process requires and what it would collide with. */
struct ProducerEnvironment {
  std::vector<ModuleConflict> conflicts;
  std::vector<MissingImport> missing;
  /** Directories that hold the producer's libraries; registered before load. */
  std::vector<std::filesystem::path> directories;
};

/**
 * @brief Lists producer-shipped libraries that the process loader would bind to an
 *        already loaded, different file with the same base name.
 *
 * The scan covers the CTI directory; when that directory is named `bin`, it covers the
 * producer root above it so sibling `lib`/`dll` trees are included. Files with identical
 * contents, compiler runtime redistributables and Windows-directory modules are not conflicts.
 */
std::vector<ModuleConflict> producer_module_conflicts(const std::filesystem::path& cti);

/**
 * @brief Resolves the CTI's static and delay-load import closure and checks it for conflicts.
 *
 * Imports resolve as a standalone producer process would: the CTI directory, then the Windows
 * system directories, then each `PATH` entry. The consumer process does not search `PATH`, so
 * every non-system directory that holds a closure library is returned for registration. A
 * different same-named file in the consumer's application directory or another returned
 * directory is a conflict, because delay loads may bind it. A delay-loaded import that cannot be
 * found would terminate the process on first use, so any missing import refuses the producer.
 * On other platforms only the directory scan applies.
 */
ProducerEnvironment inspect_producer(const std::filesystem::path& cti);

/** @brief Registers DLL search directories until the returned owner tokens are released. */
std::vector<std::shared_ptr<void>> register_producer_directories(
    const std::vector<std::filesystem::path>& directories);

/** @brief Formats conflicts as one actionable refusal message. */
std::string describe_module_conflicts(const std::filesystem::path& cti,
                                      const std::vector<ModuleConflict>& conflicts);

/** @brief Formats conflicts and missing imports as one actionable refusal message. */
std::string describe_producer_refusal(const std::filesystem::path& cti,
                                      const ProducerEnvironment& environment);

}  // namespace onegen::detail
