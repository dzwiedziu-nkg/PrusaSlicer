#pragma once

#include <cstddef>
#include <functional>

#include "Slic3r/Domain/GCodeExtrusionRole.hpp"

/**
 * @brief Extension point that decides whether a single extrusion is printed at all.
 *
 * Fill patterns clipped against a contour leave stubs behind: an infill line that
 * survives only a millimetre in a corner, an internal perimeter reduced to a sliver.
 * Each of them still costs a travel there, a retraction and a travel back, which can
 * add up to far more head movement than the extrusion itself is worth.
 *
 * A predicate installed on the Print (Biz::Slicing::IPrint::extrusion_filter) is asked
 * about every perimeter and every infill path as the extrusions of a layer are
 * collected, before travels and seams are decided, so a rejected path disappears
 * together with the movement that would have reached it.
 *
 * With no predicate installed every path is kept, so the default behaviour is
 * unchanged.
 *
 * Threading: the predicate is invoked from the G-code layer generator, which is a
 * tbb::filter_mode::serial_in_order pipeline stage, so calls belonging to one Print
 * are serialized and arrive in layer order. Several Print instances may be exported
 * concurrently, so a predicate must not be shared between them unless it is thread
 * safe.
 */
namespace Slic3r::GCode::ExtrusionFilter {

/**
 * @brief One extrusion path offered to the predicate.
 */
struct PathInfo
{
    Domain::GCodeExtrusionRole role;
    /** Length of the path along its own geometry, in millimetres. */
    double length;
    std::size_t layer_id;
    /** Print Z of the layer, in millimetres. */
    double print_z;
    unsigned extruder_id;
};

/**
 * @brief Returns true to print @p path, false to drop it.
 *
 * A predicate that throws is not expected; the caller treats an empty predicate as
 * "keep everything".
 */
using Predicate = std::function<bool(const PathInfo& path)>;

/**
 * @brief Asks @p predicate about @p path, keeping it when no predicate is installed.
 */
bool keep(const Predicate& predicate, const PathInfo& path);

} // namespace Slic3r::GCode::ExtrusionFilter
