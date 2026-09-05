#ifndef slic3r_GCode_LabelObjects_hpp_
#define slic3r_GCode_LabelObjects_hpp_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Slic3r/Domain/GCodeFlavor.hpp"

#include "libslic3r/GCode/ObjectLabels.hpp"
#include "libslic3r/Print.hpp"

namespace Slic3r {

class PrintInstance;
class Print;
class GCodeWriter;

namespace GCode {

class LabelObjects
{
public:
    void init(const SpanOfConstPtrs<PrintObject>& objects, Domain::LabelObjectsStyle label_object_style, Domain::GCodeFlavor gcode_flavor);

    /**
     * @brief Lists a part of the print that is not a model object among the objects.
     *
     * Call between init() and all_objects_header(), so the definition reaches the
     * printer with the rest of the list. Does nothing when labelling is switched off.
     * @p outline is its footprint on the bed, scaled, in G-code coordinates.
     */
    void add_region(
        ObjectLabels::RegionKind kind, const std::string& name, const Domain::Polygon& outline
    );

    /** @brief Whether add_region() has listed that region. */
    bool has_region(ObjectLabels::RegionKind kind) const;

    std::string all_objects_header() const;
    std::string all_objects_header_singleline_json() const;

    bool update(const PrintInstance *instance);

    std::string maybe_start_instance(GCodeWriter& writer);

    std::string maybe_stop_instance();

    std::string maybe_change_instance(GCodeWriter& writer);

    /**
     * @brief Opens the label of a listed region, closing whatever label was open.
     *
     * Empty when that region was not listed, so with no strategy installed every call
     * site is a no-op and the G-code is the stock G-code.
     *
     * No G92 goes with it, unlike maybe_start_instance(): the one region there is today
     * only exists on prints the slicer requires relative E distances for, where there is
     * no extrusion origin to reset.
     */
    std::string start_region(ObjectLabels::RegionKind kind);

    /** @brief Closes the label opened by start_region(). */
    std::string stop_region();

    /**
     * @brief The pair that closes an open region label and opens it again, as text.
     *
     * For G-code that has to sit among a region's extrusions but outside the region: the
     * tool change spliced into the middle of the wipe tower has to run even when the
     * tower has been cancelled, or the rest of the layer prints in the wrong filament.
     * Leaves what is open alone, because the pair restores it.
     */
    std::pair<std::string, std::string> suspend_region(ObjectLabels::RegionKind kind) const;

    bool has_active_instance();

private:
    struct LabelData
    {
        /** @brief Null for a listed region, which belongs to no model object. */
        const PrintInstance* pi;
        /** @brief Set for a listed region, empty for a model object. */
        std::optional<ObjectLabels::RegionKind> kind;
        std::string name;
        std::string center;
        std::string polygon;
        int unique_id;
    };

    enum class IncludeName {
        No,
        Yes
    };

    std::string start_object(const LabelData& label, IncludeName include_name) const;
    std::string stop_object(const LabelData& label) const;

    const LabelData* find(const PrintInstance& print_instance) const;
    const LabelData* find(ObjectLabels::RegionKind kind) const;

    /** @brief The print instance of the open label, null when none or when a region. */
    const PrintInstance* current_instance() const;

    /** @brief The label currently open, object or region alike. */
    const LabelData* current_label{nullptr};
    const PrintInstance* last_operation_instance{nullptr};

    Domain::LabelObjectsStyle m_label_objects_style;
    Domain::GCodeFlavor       m_flavor;
    std::vector<LabelData> m_label_data;
};
} // namespace GCode
} // namespace Slic3r

#endif // slic3r_GCode_LabelObjects_hpp_
