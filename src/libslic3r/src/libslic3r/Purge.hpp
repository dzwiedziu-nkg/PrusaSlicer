#ifndef slic3r_Purge_hpp_
#define slic3r_Purge_hpp_

#include "libslic3r/ExtrusionEntity.hpp"

namespace Slic3r {

class Flow;
class Layer;

/**
 * @brief Plain extrusion laid in the empty space a layer's own infill leaves.
 *
 * Some things the slicer does leave the nozzle in an unknown state: an extra pass that starves
 * the extruder for minutes, a pause the printer hangs through, a colour change that parks the
 * head somewhere and lets it drip. All of them are fixed the same way - push plain plastic
 * through the nozzle until the melt is one it can predict again - and all of them need
 * somewhere to put it.
 *
 * A wipe tower is the usual answer and it is expensive: it wants room on the bed and it has to
 * be printed from the first layer up. The room between a layer's own sparse infill lines costs
 * neither. It is inside the part, it is at the layer's own Z so nothing stands proud for the
 * next layer's nozzle to hit, and what goes there is not waste but extra material in the
 * object.
 *
 * This is only possible after the object's own extrusions for the layer exist, which is to say
 * after posInfill. Anything running earlier has nothing to find room between.
 */
namespace Purge {

/**
 * @brief Fills the room @p layer has to spare with plain extrusion, up to @p volume mm3.
 *
 * Room is what the slicer meant to fill, less what it actually put there, less half a bead so
 * a purge line never lands on material that is already down. Whole lines are taken until the
 * budget is met, because a purge is a quantity of plastic and not a pattern; a layer with less
 * room than that gives what it has, and the caller is told how much that was by measuring what
 * comes back.
 *
 * The paths are laid with @p role so the G-code stage can tell them from the layer's own work.
 */
ExtrusionEntitiesPtr generate(
    const Layer  &layer,
    const Flow   &flow,
    double        volume,
    ExtrusionRole role = ExtrusionRole::SolidInfill
);

/** @brief How much plastic a set of purge paths carries, in mm3. */
double volume_of(const ExtrusionEntitiesPtr &paths);

/**
 * @brief How much room @p layer has to spare, in mm2.
 *
 * The same measurement generate() works from, offered on its own so that a caller can decide
 * how much to ask for before asking for it.
 */
double spare_area(const Layer &layer, const Flow &flow);

} // namespace Purge
} // namespace Slic3r

#endif // slic3r_Purge_hpp_
