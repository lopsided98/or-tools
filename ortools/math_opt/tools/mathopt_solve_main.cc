// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tool to run MathOpt on the given problems.
//
// Examples:
//  * Solve a model stored as a proto (infer the file/proto type):
//      mathopt_solve --input_file model.pb
//  * Solve a gzipped mps file, pick your solver:
//      mathopt_solve --input_file model.mps.gz --solver_type=glop
//  * Set a time limit:
//      mathopt_solve --input_file model.pb --time_limit 10s
//  * Set solve parameters in proto text format (see parameters.proto):
//      mathopt_solve --input_file model.pb --solve_parameters 'threads: 4'
//  * Specify the file format:
//      mathopt_solve --input_file model --format=mathopt
// MOE: begin_strip
//  * Solve a MIPLIB problem:
//      mathopt_solve --input_file
//      /google/src/head/depot/operations_research_data/MIP_MIPLIB/miplib2017/10teams.mps.gz
// MOE: end_strip
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "google/protobuf/text_format.h"
#include "ortools/base/helpers.h"
#include "ortools/base/init_google.h"
#include "ortools/base/logging.h"
#include "ortools/base/options.h"
#include "ortools/base/status_macros.h"
#include "ortools/math_opt/core/solver_interface.h"
#include "ortools/math_opt/cpp/math_opt.h"
#include "ortools/math_opt/cpp/statistics.h"
#include "ortools/math_opt/io/names_removal.h"
#include "ortools/math_opt/io/proto_converter.h"
#include "ortools/math_opt/labs/solution_feasibility_checker.h"
#include "ortools/math_opt/parameters.pb.h"
#include "ortools/math_opt/tools/file_format_flags.h"
#include "ortools/util/status_macros.h"

namespace {

struct SolverTypeProtoFormatter {
  void operator()(
      std::string* const out,
      const operations_research::math_opt::SolverTypeProto solver_type) {
    out->append(EnumToString(EnumFromProto(solver_type).value()));
  }
};

}  // namespace

ABSL_FLAG(std::string, input_file, "",
          "the file containing the model to solve; use --format to specify the "
          "file format");
ABSL_FLAG(
    std::optional<operations_research::math_opt::FileFormat>, format,
    std::nullopt,
    absl::StrCat(
        "the format of the --input_file; possible values:",
        operations_research::math_opt::OptionalFormatFlagPossibleValuesList()));
ABSL_FLAG(
    std::vector<std::string>, update_files, {},
    absl::StrCat(
        "the file containing ModelUpdateProto to apply to the --input_file; "
        "when this flag is used, the --format must be either ",
        AbslUnparseFlag(
            operations_research::math_opt::FileFormat::kMathOptBinary),
        " or  ",
        AbslUnparseFlag(
            operations_research::math_opt::FileFormat::kMathOptText)));

ABSL_FLAG(operations_research::math_opt::SolverType, solver_type,
          operations_research::math_opt::SolverType::kGscip,
          absl::StrCat(
              "the solver to use, possible values: ",
              absl::StrJoin(
                  operations_research::math_opt::AllSolversRegistry::Instance()
                      ->RegisteredSolvers(),
                  ", ", SolverTypeProtoFormatter())));
ABSL_FLAG(operations_research::math_opt::SolveParameters, solve_parameters, {},
          "SolveParameters in text-proto format. Note that the time limit is "
          "overridden by the --time_limit flag.");
ABSL_FLAG(bool, solver_logs, false,
          "use a message callback to print the solver convergence logs");
ABSL_FLAG(absl::Duration, time_limit, absl::InfiniteDuration(),
          "the time limit to use for the solve");

ABSL_FLAG(bool, names, true,
          "use the names in the input models; ignoring names is useful when "
          "the input contains duplicates");
ABSL_FLAG(bool, ranges, false,
          "prints statistics about the ranges of the model values");
ABSL_FLAG(bool, print_model, false, "prints the model to stdout");
ABSL_FLAG(bool, lp_relaxation, false,
          "relax all integer variables to continuous");
ABSL_FLAG(
    bool, check_solutions, false,
    "check the solutions feasibility; use --absolute_constraint_tolerance, "
    "--integrality_tolerance, and --nonzero_tolerance values for tolerances");
ABSL_FLAG(double, absolute_constraint_tolerance,
          operations_research::math_opt::FeasibilityCheckerOptions{}
              .absolute_constraint_tolerance,
          "feasibility tolerance for constraints and variables bounds");
ABSL_FLAG(double, integrality_tolerance,
          operations_research::math_opt::FeasibilityCheckerOptions{}
              .integrality_tolerance,
          "feasibility tolerance for variables' integrality");
ABSL_FLAG(
    double, nonzero_tolerance,
    operations_research::math_opt::FeasibilityCheckerOptions{}
        .nonzero_tolerance,
    "tolerance for checking if a value is nonzero (e.g., in SOS constraints)");

namespace operations_research::math_opt {
namespace {

// Returns the ModelUpdateProto read from the given file. The format must be
// kMathOptBinary or kMathOptText; other values will generate an error.
absl::StatusOr<ModelUpdateProto> ReadModelUpdate(
    const absl::string_view file_path, const FileFormat format) {
  switch (format) {
    case FileFormat::kMathOptBinary:
      return file::GetBinaryProto<ModelUpdateProto>(file_path,
                                                    file::Defaults());
    case FileFormat::kMathOptText:
      return file::GetTextProto<ModelUpdateProto>(file_path, file::Defaults());
    default:
      return util::InternalErrorBuilder() << "invalid format " << format;
  }
}

// Prints the summary of the solve result.
//
// If feasibility_check_tolerances is not nullopt then a check of feasibility of
// solution is done with the provided tolerances.
absl::Status PrintSummary(const Model& model, const SolveResult& result,
                          const std::optional<FeasibilityCheckerOptions>
                              feasibility_check_tolerances) {
  std::cout << "Solve finished:\n"
            << "  termination: " << result.termination << "\n"
            << "  solve time: " << result.solve_stats.solve_time
            << "\n  best primal bound: " << result.solve_stats.best_primal_bound
            << "\n  best dual bound: " << result.solve_stats.best_dual_bound
            << std::endl;
  if (result.solutions.empty()) {
    std::cout << "  no solution" << std::endl;
  }
  for (int i = 0; i < result.solutions.size(); ++i) {
    const Solution& solution = result.solutions[i];
    std::cout << "  solution #" << (i + 1) << " objective: ";
    if (solution.primal_solution.has_value()) {
      std::cout << solution.primal_solution->objective_value;
      if (feasibility_check_tolerances.has_value()) {
        OR_ASSIGN_OR_RETURN3(
            const ModelSubset broken_constraints,
            CheckPrimalSolutionFeasibility(
                model, solution.primal_solution->variable_values,
                *feasibility_check_tolerances),
            _ << "failed to check the primal solution feasibility of solution #"
              << (i + 1));
        if (!broken_constraints.empty()) {
          std::cout << " (numerically infeasible: " << broken_constraints
                    << ')';
        } else {
          std::cout << " (numerically feasible)";
        }
      }
    } else {
      std::cout << "n/a";
    }
    std::cout << std::endl;
  }

  return absl::OkStatus();
}

absl::Status RunSolver() {
  const std::string input_file_path = absl::GetFlag(FLAGS_input_file);
  if (input_file_path.empty()) {
    LOG(QFATAL) << "The flag --input_file is mandatory.";
  }

  // Parses --format.
  const FileFormat format = [&]() {
    const std::optional<FileFormat> format =
        FormatFromFlagOrFilePath(absl::GetFlag(FLAGS_format), input_file_path);
    if (format.has_value()) {
      return *format;
    }
    LOG(QFATAL) << "Can't guess the format from the file extension, please "
                   "use --format to specify the file format explicitly.";
  }();
  // We deal with input validation in the ReadModel() function.

  // Read the model and the optional updates.
  const std::vector<std::string> update_file_paths =
      absl::GetFlag(FLAGS_update_files);
  if (!update_file_paths.empty() && format != FileFormat::kMathOptBinary &&
      format != FileFormat::kMathOptText) {
    LOG(QFATAL) << "Can't use --update_files with a input of format " << format
                << ".";
  }

  OR_ASSIGN_OR_RETURN3((auto [model_proto, optional_hint]),
                       ReadModel(input_file_path, format),
                       _ << "failed to read " << input_file_path);

  std::vector<ModelUpdateProto> model_updates;
  for (const std::string& update_file_path : update_file_paths) {
    ASSIGN_OR_RETURN(ModelUpdateProto update,
                     ReadModelUpdate(update_file_path, format));
    model_updates.emplace_back(std::move(update));
  }

  if (!absl::GetFlag(FLAGS_names)) {
    RemoveNames(model_proto);
    for (ModelUpdateProto& update : model_updates) {
      RemoveNames(update);
    }
  }

  // Parse the problem and the updates.
  ASSIGN_OR_RETURN(const std::unique_ptr<Model> model,
                   Model::FromModelProto(model_proto));
  for (int u = 0; u < model_updates.size(); ++u) {
    const ModelUpdateProto& update = model_updates[u];
    RETURN_IF_ERROR(model->ApplyUpdateProto(update))
        << "failed to apply the update file: " << update_file_paths[u];
  }
  if (absl::GetFlag(FLAGS_lp_relaxation)) {
    for (const Variable v : model->Variables()) {
      model->set_continuous(v);
    }
  }

  if (absl::GetFlag(FLAGS_ranges)) {
    std::cout << "Ranges of finite non-zero values in the model:\n"
              << ComputeModelRanges(*model) << std::endl;
  }

  // Optionally prints the problem.
  if (absl::GetFlag(FLAGS_print_model)) {
    std::cout << *model;
    std::cout.flush();
  }

  // Solve the problem.
  SolveParameters solve_parameters = absl::GetFlag(FLAGS_solve_parameters);
  solve_parameters.time_limit = absl::GetFlag(FLAGS_time_limit);
  SolveArguments solve_args = {.parameters = solve_parameters};
  if (optional_hint.has_value()) {
    OR_ASSIGN_OR_RETURN3(ModelSolveParameters::SolutionHint hint,
                         ModelSolveParameters::SolutionHint::FromProto(
                             *model, optional_hint.value()),
                         _ << "invalid solution hint");
    solve_args.model_parameters.solution_hints.push_back(std::move(hint));
    std::cout << "Using the solution hint from the MPModelProto." << std::endl;
  }
  if (absl::GetFlag(FLAGS_solver_logs)) {
    solve_args.message_callback = PrinterMessageCallback(std::cout, "logs| ");
  }
  OR_ASSIGN_OR_RETURN3(
      const SolveResult result,
      Solve(*model, absl::GetFlag(FLAGS_solver_type), solve_args),
      _ << "the solver failed");

  const FeasibilityCheckerOptions feasiblity_checker_options = {
      .absolute_constraint_tolerance =
          absl::GetFlag(FLAGS_absolute_constraint_tolerance),
      .integrality_tolerance = absl::GetFlag(FLAGS_integrality_tolerance),
      .nonzero_tolerance = absl::GetFlag(FLAGS_nonzero_tolerance),
  };
  RETURN_IF_ERROR(
      PrintSummary(*model, result,
                   absl::GetFlag(FLAGS_check_solutions)
                       ? std::make_optional(feasiblity_checker_options)
                       : std::nullopt));

  return absl::OkStatus();
}

}  // namespace
}  // namespace operations_research::math_opt

int main(int argc, char* argv[]) {
  InitGoogle(argv[0], &argc, &argv, /*remove_flags=*/true);

  const absl::Status status = operations_research::math_opt::RunSolver();
  // We don't use QCHECK_OK() here since the logged message contains more than
  // the failing status.
  if (!status.ok()) {
    LOG(QFATAL) << status;
  }

  return 0;
}
