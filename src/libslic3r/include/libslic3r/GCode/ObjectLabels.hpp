#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Slic3r/Domain/Polygon.hpp"

/**
 * @brief Extension point that names the parts of a print that are not model objects.
 *
 * Firmware that can cancel an object during a print - M486 on Marlin and Prusa Buddy,
 * EXCLUDE_OBJECT on Klipper - works from a list the slicer writes into the G-code, and
 * that list holds model objects only. Everything else the printer lays down belongs to
 * no object and cannot be cancelled: the wipe tower keeps being built after the last
 * object that needed it has been cancelled, for as many hours as the print had left.
 *
 * Whether such a part should be cancellable is a judgement about the print rather than
 * a fact about the geometry, so a Strategy installed on the Print
 * (Biz::Slicing::IPrint::object_labels) is offered every part the slicer knows is not a
 * model object, once per export, and answers with the name to list it under or with
 * nothing to leave it as it is. With no strategy installed nothing is named and the
 * G-code is the stock G-code.
 *
 * The engine owns the syntax: a named region is defined in the same header, in the same
 * dialect and with the same identifiers as the objects around it, and its label is
 * opened and closed around its extrusions. What a strategy chooses is which parts get a
 * label and what it says.
 *
 * Threading: the strategy is called from the G-code export, once per region, before any
 * layer is generated. Several Print instances may be exported concurrently, so a
 * strategy must not be shared between them unless it is thread safe.
 */
namespace Slic3r::GCode::ObjectLabels {

/** @brief A part of the print the slicer prints but does not call an object. */
enum class RegionKind {
    /** @brief The tower purged into on every tool change. */
    WipeTower
};

/** @brief One such part, offered to the strategy. */
struct RegionInfo
{
    RegionKind kind;
    /** @brief What the slicer calls it, as a starting point for a name. */
    std::string default_name;
    /**
     * @brief Its footprint on the bed, scaled, in G-code coordinates.
     *
     * The same frame the object outlines in the label header are given in, so a strategy
     * may compare them.
     */
    Domain::Polygon outline;
};

/**
 * @brief Answers with the name to make @p region cancellable under, or nothing.
 *
 * An empty or blank name is treated as nothing. The name reaches the firmware, which on
 * some flavours restricts what may appear in it, so the engine sanitizes it the same way
 * it sanitizes an object name.
 */
using Strategy = std::function<std::optional<std::string>(const RegionInfo& region)>;

/**
 * @brief Asks @p strategy about @p region, naming nothing when none is installed.
 */
std::optional<std::string> name_for(const Strategy& strategy, const RegionInfo& region);

} // namespace Slic3r::GCode::ObjectLabels
