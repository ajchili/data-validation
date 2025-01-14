/* Copyright 2018 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_data_validation/anomalies/feature_util.h"

#include <string>

#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "tensorflow_data_validation/anomalies/metrics.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow_metadata/proto/v0/anomalies.pb.h"

namespace tensorflow {
namespace data_validation {
namespace {
using absl::optional;
using ::tensorflow::metadata::v0::Feature;
using tensorflow::metadata::v0::FeatureComparator;
using ::tensorflow::metadata::v0::SparseFeature;

constexpr char kSuperfluousValues[] = "Superfluous values";
constexpr char kMissingValues[] = "Missing values";
constexpr char kDropped[] = "Column dropped";

ComparatorContext GetContext(FeatureComparatorType comparator_type) {
  switch (comparator_type) {
    case FeatureComparatorType::SKEW:
      return {"serving", "training"};
    case FeatureComparatorType::DRIFT:
      return {"previous", "current"};
  }
}

// TODO(martinz): These functions show that the interface in
// FeatureStatsView was "too cute", in that I might as well get the appropriate
// control dataset view, and then get the right FeatureStatsView.
// Should probably remove FeatureStatsView::GetServing and
// FeatureStatsView::GetPrevious.
bool HasControlDataset(const FeatureStatsView& stats,
                       FeatureComparatorType comparator_type) {
  switch (comparator_type) {
    case FeatureComparatorType::SKEW:
      return stats.parent_view().GetServing() != absl::nullopt;
    case FeatureComparatorType::DRIFT:
      return stats.parent_view().GetPreviousSpan() != absl::nullopt;
  }
}

absl::optional<FeatureStatsView> GetControlStats(
    const FeatureStatsView& stats, FeatureComparatorType comparator_type) {
  switch (comparator_type) {
    case FeatureComparatorType::SKEW:
      return stats.GetServing();
    case FeatureComparatorType::DRIFT:
      return stats.GetPreviousSpan();
  }
}

}  // namespace

std::vector<Description> UpdateValueCount(
    const FeatureStatsView& feature_stats_view,
    tensorflow::metadata::v0::ValueCount* value_count) {
  DCHECK_NE(value_count, nullptr);

  std::vector<Description> description;
  if (value_count->has_min() &&
      feature_stats_view.min_num_values() < value_count->min()) {
    description.push_back(
        {tensorflow::metadata::v0::AnomalyInfo::FEATURE_TYPE_LOW_NUMBER_VALUES,
         kMissingValues, "Some examples have fewer values than expected."});
    if (feature_stats_view.min_num_values() == 0) {
      value_count->clear_min();
    } else {
      value_count->set_min(feature_stats_view.min_num_values());
    }
  }

  if (value_count->has_max() &&
      feature_stats_view.max_num_values() > value_count->max()) {
    description.push_back(
        {tensorflow::metadata::v0::AnomalyInfo::FEATURE_TYPE_HIGH_NUMBER_VALUES,
         kSuperfluousValues, "Some examples have more values than expected."});
    value_count->set_max(feature_stats_view.max_num_values());
  }
  return description;
}

bool FeatureHasComparator(const Feature& feature,
                          FeatureComparatorType comparator_type) {
  switch (comparator_type) {
    case FeatureComparatorType::DRIFT:
      return feature.has_drift_comparator();
    case FeatureComparatorType::SKEW:
      return feature.has_skew_comparator();
  }
}

tensorflow::metadata::v0::FeatureComparator* GetFeatureComparator(
    Feature* feature, FeatureComparatorType comparator_type) {
  switch (comparator_type) {
    case FeatureComparatorType::DRIFT:
      return feature->mutable_drift_comparator();
    case FeatureComparatorType::SKEW:
      return feature->mutable_skew_comparator();
  }
}

namespace {

// Templated implementations for [Feature, SparseFeature].

template <class T>
void DeprecateFeatureType(T* feature) {
  DCHECK_NE(feature, nullptr);
  feature->set_lifecycle_stage(tensorflow::metadata::v0::DEPRECATED);
}

template <class T>
bool FeatureTypeIsDeprecated(const T& feature) {
  if (feature.deprecated()) {  // NOLINT
    return true;
  }
  if (feature.has_lifecycle_stage()) {
    switch (feature.lifecycle_stage()) {
      case tensorflow::metadata::v0::PLANNED:
      case tensorflow::metadata::v0::ALPHA:
      case tensorflow::metadata::v0::DEPRECATED:
      case tensorflow::metadata::v0::DEBUG_ONLY:
        return true;
      case tensorflow::metadata::v0::UNKNOWN_STAGE:
      case tensorflow::metadata::v0::BETA:
      case tensorflow::metadata::v0::PRODUCTION:
      default:
        return false;
    }
  }
  return false;
}

}  // namespace

void DeprecateFeature(Feature* feature) {
  return DeprecateFeatureType(feature);
}

void DeprecateSparseFeature(SparseFeature* sparse_feature) {
  return DeprecateFeatureType(sparse_feature);
}

bool FeatureIsDeprecated(const Feature& feature) {
  return FeatureTypeIsDeprecated(feature);
}

bool SparseFeatureIsDeprecated(const SparseFeature& sparse_feature) {
  return FeatureTypeIsDeprecated(sparse_feature);
}

std::vector<Description> UpdateFeatureComparatorDirect(
    const FeatureStatsView& stats, const FeatureComparatorType comparator_type,
    tensorflow::metadata::v0::FeatureComparator* comparator) {
  if (!comparator->infinity_norm().has_threshold()) {
    // There is nothing to check.
    return {};
  }
  const ComparatorContext& context = GetContext(comparator_type);
  absl::optional<FeatureStatsView> control_stats =
      GetControlStats(stats, comparator_type);
  if (control_stats) {
    const double threshold = comparator->infinity_norm().threshold();
    const std::pair<string, double> distance =
        LInftyDistance(stats, *control_stats);
    const string max_difference_value = distance.first;
    const double stats_infinity_norm = distance.second;
    if (stats_infinity_norm > threshold) {
      comparator->mutable_infinity_norm()->set_threshold(stats_infinity_norm);
      return {
          {tensorflow::metadata::v0::AnomalyInfo::COMPARATOR_L_INFTY_HIGH,
           absl::StrCat("High Linfty distance between ", context.treatment_name,
                        " and ", context.control_name),
           absl::StrCat("The Linfty distance between ", context.treatment_name,
                        " and ", context.control_name, " is ",
                        absl::SixDigits(stats_infinity_norm),
                        " (up to six significant digits), above the threshold ",
                        absl::SixDigits(threshold),
                        ". The feature value with maximum difference is: ",
                        max_difference_value)}};
    }

  } else if (HasControlDataset(stats, comparator_type)) {
    // If there is a control dataset, but that dataset does not contain
    // statistics for the feature at issue, generate a missing control data
    // anomaly.
    // TODO(martinz): Consider clearing entire object.
    comparator->mutable_infinity_norm()->clear_threshold();
    return {
        {tensorflow::metadata::v0::AnomalyInfo::COMPARATOR_CONTROL_DATA_MISSING,
         absl::StrCat(context.control_name, " data missing"),
         absl::StrCat(context.control_name, " data is missing.")}};
  }
  return {};
}

double GetMaxOffDomain(const tensorflow::metadata::v0::DistributionConstraints&
                           distribution_constraints) {
  return distribution_constraints.has_min_domain_mass()
             ? (1.0 - distribution_constraints.min_domain_mass())
             : 0.0;
}

// TODO(b/117184825): Currently the clear_oneof_name() method is private.
// Clean up the code to use this method if it is made public or if there is a
// better way to clear oneof fields.
void ClearDomain(Feature* feature) {
  feature->mutable_int_domain();
  // Note that this clears the oneof field domain_info.
  feature->clear_int_domain();
}

void InitValueCountAndPresence(const FeatureStatsView& feature_stats_view,
                               Feature* feature) {
  double num_present = feature_stats_view.GetNumPresent();
  if (num_present < 1.0) {
    // Note that we also set min_count to be zero when num_present is between
    // (0.0, 1.0)
    feature->mutable_presence()->set_min_count(0);
  } else {
    feature->mutable_presence()->set_min_count(1);
  }

  // If there are no examples containing this feature, do not infer value
  // counts.
  if (num_present == 0.0) {
    return;
  }
  if (feature_stats_view.GetNumMissing() == 0.0) {
    // Required feature.
    feature->mutable_presence()->set_min_fraction(1.0);
  }
  if (feature_stats_view.min_num_values() > 0) {
    if (feature_stats_view.min_num_values() ==
        feature_stats_view.max_num_values()) {
      // Set min and max value count in the schema if they are same. This would
      // allow required features with same valency to be parsed as dense tensors
      // in TFT.
      feature->mutable_value_count()->set_min(
          feature_stats_view.min_num_values());
      feature->mutable_value_count()->set_max(
          feature_stats_view.min_num_values());
    } else {
      feature->mutable_value_count()->set_min(1);
    }
  }
}

std::vector<Description> UpdatePresence(
    const FeatureStatsView& feature_stats_view,
    tensorflow::metadata::v0::FeaturePresence* presence) {
  std::vector<Description> descriptions;
  const optional<double> num_present = feature_stats_view.GetNumPresent();
  if (presence->has_min_count() && num_present) {
    if (*num_present < presence->min_count()) {
      presence->set_min_count(*num_present);
      descriptions.push_back(
          {tensorflow::metadata::v0::AnomalyInfo::
               FEATURE_TYPE_LOW_NUMBER_PRESENT,
           kDropped,
           "The feature was present in fewer examples than expected."});
    }
  }
  const optional<double> fraction_present =
      feature_stats_view.GetFractionPresent();
  if (presence->has_min_fraction() && fraction_present) {
    if (*fraction_present < presence->min_fraction()) {
      presence->set_min_fraction(*fraction_present);
      descriptions.push_back(
          {tensorflow::metadata::v0::AnomalyInfo::
               FEATURE_TYPE_LOW_FRACTION_PRESENT,
           kDropped,
           "The feature was present in fewer examples than expected."});
    }
    if (presence->min_fraction() == 1.0) {
      if (feature_stats_view.GetNumMissing() != 0.0) {
        // In this case, there is a very small fraction of examples missing,
        // such that floating point error can hide it. We treat this case
        // separately, and set a threshold that is numerically distant from
        // 1.0.
        // TODO(martinz): update the anomaly type here to be unique.
        presence->set_min_fraction(0.9999);
        descriptions.push_back(
            {tensorflow::metadata::v0::AnomalyInfo::
                 FEATURE_TYPE_LOW_FRACTION_PRESENT,
             kDropped,
             absl::StrCat(
                 "The feature was expected everywhere, but was missing in ",
                 feature_stats_view.GetNumMissing(), " examples.")});
      }
    }
  }
  return descriptions;
}

}  // namespace data_validation
}  // namespace tensorflow
