#pragma once

#include <functional>
#include <optional>
#include <variant>
#include <vector>
#include "Slic3r/Domain/BedInstance.hpp"
#include "Slic3r/Domain/ConfigPack.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/Preset/SelectedPreset.hpp"
#include "jthread/JThread.hpp"
#include "libslic3r/GCode/ExtrusionFilter.hpp"
#include "libslic3r/Fill/FillPlanner.hpp"
#include "libslic3r/GCode/IslandOrdering.hpp"
#include "libslic3r/GCode/IslandSequencing.hpp"
#include "libslic3r/GCode/ObjectLabels.hpp"
#include "libslic3r/IThumbnailImageGenerator.hpp"
#include "libslic3r/PassPlanner.hpp"
#include "libslic3r/PrintSteps.hpp"
#include "libslic3r/SlicingStatus.hpp"
#include "Slic3r/Domain/PrintStatistics.hpp"
#include "Slic3r/Domain/SLA/PrintStatistics.hpp"
#include "Slic3r/Domain/SlicingId.hpp"
#include "libslic3r/SerializedConfig.hpp"

namespace Slic3r::Biz::Slicing {

namespace ApplyStatus {
struct Unchanged
{};

struct Empty
{};

struct Changed
{
    std::vector<Warning> warrnings;
};

struct InvalidData
{
    std::vector<Error> errors;
};

using Status = std::variant<InvalidData, Unchanged, Changed, Empty>;
} // namespace ApplyStatus

using PrintObjectStep = std::variant<FDMPrintObjectStep, SLAPrintObjectStep>;

// Restricts the slicing to the single model object and stops the slicing
// after the given print object step is finished.
struct SliceUntilStep
{
    PrintObjectStep step;
    Domain::ObjectID model_object_id;

    bool operator==(const SliceUntilStep&) const = default;
};

class IPrint
{
public:
    using UniversalPrintStatistics =
        std::variant<Domain::PrintStatistics, Domain::SLA::PrintStatistics>;
    using MetadataSerializeFn = std::function<SerializedConfig(const UniversalPrintStatistics&)>;

    virtual ApplyStatus::Status update(Domain::Model& model,
                                       const Domain::ConfigPack& config,
                                       const Domain::BedInstance& bed,
                                       const Domain::Preset::SelectedPresetMetadata& metadata,
                                       const MetadataSerializeFn& serializer) = 0;
    virtual void slice(
        Domain::SlicingId,
        Slicing::IThumbnailImageGenerator&,
        std::optional<SliceUntilStep> slice_until_step
    )                          = 0;
    virtual bool empty() const = 0;
    virtual ~IPrint()          = default;

    JThread::StopToken stop_token;
    std::function<void(Biz::Slicing::Progress)> progress_callback{[](Biz::Slicing::Progress) {}};
    std::function<void(Biz::Slicing::Warning)> append_warning_callback{
        [](Biz::Slicing::Warning) {}};

    /**
     * Decides the print order of the islands of a layer during G-code export.
     * Empty by default, which keeps the order chained at slicing time. Only used by
     * FFF prints. See GCode::IslandOrdering for the contract and the threading rules.
     */
    GCode::IslandOrdering::Strategy island_ordering_strategy;

    /**
     * Decides whether a single extrusion is printed at all, asked about every
     * perimeter and infill path during G-code export. Empty by default, which keeps
     * every path. Only used by FFF prints. See GCode::ExtrusionFilter.
     */
    GCode::ExtrusionFilter::Predicate extrusion_filter;

    /**
     * Generates the fill paths of a surface in place of the stock pattern, asked about
     * every surface during slicing. Empty by default, which keeps the stock paths. Only
     * used by FFF prints. See FillPlanner - note that unlike the G-code hooks this one
     * is called concurrently from several threads.
     */
    FillPlanner::Strategy fill_planner;

    /**
     * Plans an extra pass over an area the slicer has just covered - ironing a support
     * interface flat, for instance - asked about every support interface surface during
     * slicing. Empty by default, which runs no extra pass anywhere. Only used by FFF
     * prints. See PassPlanner - like the fill planner and unlike the G-code hooks, this
     * one is called concurrently from several threads.
     */
    PassPlanner::Strategy pass_planner;

    /**
     * Optionally schedules connected islands across layer boundaries. Empty by
     * default, which preserves normal layer-by-layer G-code generation.
     */
    GCode::IslandSequencing::Strategy island_sequencing_strategy;

    /**
     * Names the parts of the print that are not model objects, so the printer can
     * cancel them during a print the way it cancels an object. Asked once per part
     * during G-code export. Empty by default, which names none of them and leaves the
     * object list exactly as the stock slicer writes it. See GCode::ObjectLabels.
     */
    GCode::ObjectLabels::Strategy object_labels;
};

struct ValidationResult
{
    std::vector<Biz::Slicing::Error> errors;
    std::vector<Biz::Slicing::Warning> warnings;
};
} // namespace Slic3r::Biz::Slicing
