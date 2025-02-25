#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Milling/MillingPostProcess.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "Surface.hpp"
#include "BoundingBox.hpp"
#include "SVG.hpp"

#include <string>
#include <map>

#include <boost/log/trivial.hpp>

namespace Slic3r {

Flow LayerRegion::flow(FlowRole role, bool bridge, double width) const
{
    return m_region->flow(
        role,
        m_layer->height,
        bridge,
        m_layer->id() == 0,
        width,
        *m_layer->object()
    );
}

// Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
void LayerRegion::slices_to_fill_surfaces_clipped()
{
    //if (this->region()->config().no_perimeter_full_bridge) return;
    // Note: this method should be idempotent, but fill_surfaces gets modified 
    // in place. However we're now only using its boundaries (which are invariant)
    // so we're safe. This guarantees idempotence of prepare_infill() also in case
    // that combine_infill() turns some fill_surface into VOID surfaces.
//    Polygons fill_boundaries = to_polygons(std::move(this->fill_surfaces));
    Polygons fill_boundaries = to_polygons(this->fill_expolygons);
    // Collect polygons per surface type.
    std::map<SurfaceType, Polygons> polygons_by_surface;
    for (const Surface &surface : this->slices().surfaces) {
        polygons_append(polygons_by_surface[surface.surface_type], surface.expolygon);
    }
    // Trim surfaces by the fill_boundaries.
    this->fill_surfaces.surfaces.clear();
    for (auto const& entry : polygons_by_surface) {
        if (!entry.second.empty())
            //if (entry.first & stModBridge == stModBridge && this->region()->config().no_perimeter_full_bridge)
            //    this->fill_surfaces.append(entry.second, entry.first);
            //else
                this->fill_surfaces.append(intersection_ex(entry.second, fill_boundaries), entry.first);
    }
}

void LayerRegion::make_perimeters(const SurfaceCollection &slices, SurfaceCollection* fill_surfaces)
{
    this->perimeters.clear();
    this->thin_fills.clear();

    const PrintConfig       &print_config  = this->layer()->object()->print()->config();
    const PrintRegionConfig &region_config = this->region()->config();
    // This needs to be in sync with PrintObject::_slice() slicing_mode_normal_below_layer!
    bool spiral_vase = print_config.spiral_vase &&
        (this->layer()->id() >= region_config.bottom_solid_layers.value &&
         this->layer()->print_z >= region_config.bottom_solid_min_thickness - EPSILON);

    PerimeterGenerator g(
        // input:
        &slices,
        this->flow(frPerimeter),
        &region_config,
        &this->layer()->object()->config(),
        &print_config,
        spiral_vase,
        
        // output:
        &this->perimeters,
        &this->thin_fills,
        fill_surfaces
    );
    
    if (this->layer()->lower_layer != nullptr)
        // Cummulative sum of polygons over all the regions.
        g.lower_slices = &this->layer()->lower_layer->lslices;
    if (this->layer()->upper_layer != NULL)
        g.upper_slices = &this->layer()->upper_layer->lslices;
    
    g.layer                 = this->layer();
    g.ext_perimeter_flow    = this->flow(frExternalPerimeter);
    g.overhang_flow         = this->region()->flow(frPerimeter, -1, true, false, -1, *this->layer()->object());
    g.solid_infill_flow     = this->flow(frSolidInfill);
    
    g.process();

    this->fill_no_overlap_expolygons = g.fill_no_overlap;
}

void LayerRegion::make_milling_post_process(const SurfaceCollection& slices) {
    MillingPostProcess mill(// input:
        &slices,
        (this->layer()->lower_layer != nullptr) ? &this->layer()->lower_layer->lslices : nullptr,
        &this->region()->config(),
        &this->layer()->object()->config(),
        &this->layer()->object()->print()->config()
    );
    milling = mill.process(this->layer());
}

//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{

    coord_t max_margin = 0;
    if ((this->region()->config().perimeters > 0)) {
        max_margin = this->flow(frExternalPerimeter).scaled_width() + this->flow(frPerimeter).scaled_spacing() * (this->region()->config().perimeters.value - 1);
    }
    const Surfaces &surfaces = this->fill_surfaces.surfaces;
    const bool has_infill = this->region()->config().fill_density.value > 0.;
    coord_t margin = scale_(this->region()->config().external_infill_margin.get_abs_value(unscaled(max_margin)));
    coord_t margin_bridged = scale_(this->region()->config().bridged_infill_margin.get_abs_value(this->flow(frExternalPerimeter).width));
    //if no infill, reduce the margin for everything to only the perimeter
    if (!has_infill) {
        margin = std::min(margin, max_margin);
        margin_bridged = std::min(margin_bridged, max_margin);
    }
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // 1) Collect bottom and bridge surfaces, each of them grown by a parametrised ~3mm offset
    // for better anchoring.
    // Bottom surfaces, grown.
    Surfaces                    bottom;
    // Bridge surfaces, initialy not grown.
    Surfaces                    bridges;
    // Top surfaces, grown.
    Surfaces                    top;
    // Internal surfaces, not grown.
    Surfaces                    internal;
    // Areas, where an infill of various types (top, bottom, bottom bride, sparse, void) could be placed.
    Polygons                    fill_boundaries = to_polygons(this->fill_expolygons);
    Polygons                    lower_layer_covered_tmp;

    // Collect top surfaces and internal surfaces.
    // Collect fill_boundaries: If we're slicing with no infill, we can't extend external surfaces over non-existent infill.
    // This loop destroys the surfaces (aliasing this->fill_surfaces.surfaces) by moving into top/internal/fill_boundaries!

    {
        // Voids are sparse infills if infill rate is zero.
        Polygons voids;
        bool has_infill = this->region()->config().fill_density.value > 0.;
        for (const Surface &surface : this->fill_surfaces.surfaces) {
            if (surface.has_pos_top()) {
                // Collect the top surfaces, inflate them and trim them by the bottom surfaces.
                // This gives the priority to bottom surfaces.
                surfaces_append(top, offset_ex(surface.expolygon, double(margin), EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
            } else if (surface.has_pos_bottom() && (!surface.has_mod_bridge() || lower_layer == nullptr)) {
                // Grown by 3mm.
                surfaces_append(bottom, offset_ex(surface.expolygon, double(margin), EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
            } else if (surface.has_pos_bottom() && surface.has_mod_bridge()) {
                if (! surface.empty())
                    bridges.emplace_back(surface);
            }
            if (has_infill || !(surface.has_pos_internal())) {
                if (!surface.has_pos_external())
                    // Make a copy as the following line uses the move semantics.
                    internal.push_back(surface);
                polygons_append(fill_boundaries, std::move(surface.expolygon));
            } else{
                if (!surface.has_pos_external()){
                    if (! has_infill && lower_layer != nullptr)
                        polygons_append(voids, surface.expolygon);
                    internal.push_back(std::move(surface));
                }
                //push surface as perimeter-only inside the fill_boundaries
                if (margin_bridged > 0) {
                    ExPolygons peri_poly = diff_ex(ExPolygons() = { surface.expolygon }, offset_ex(surface.expolygon, -margin_bridged));
                    polygons_append(fill_boundaries, peri_poly);
                }
            }
        }
        if (! has_infill && lower_layer != nullptr && ! voids.empty()) {
            // Remove voids from fill_boundaries, that are not supported by the layer below.
            if (lower_layer_covered == nullptr) {
                lower_layer_covered = &lower_layer_covered_tmp;
            	lower_layer_covered_tmp = to_polygons(lower_layer->lslices);
            }
            if (! lower_layer_covered->empty())
                voids = diff(voids, *lower_layer_covered);
            fill_boundaries = diff(fill_boundaries, voids);
        }
    }

#if 0
    {
        static int iRun = 0;
        bridges.export_to_svg(debug_out_path("bridges-before-grouping-%d.svg", iRun ++), true);
    }
#endif

    if (bridges.empty())
    {
        fill_boundaries = union_(fill_boundaries, true);
    } else
    {
        // 1) Calculate the inflated bridge regions, each constrained to its island.
        ExPolygons               fill_boundaries_ex = union_ex(fill_boundaries, true);
        std::vector<Polygons>    bridges_grown;
        std::vector<BoundingBox> bridge_bboxes;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("3_process_external_surfaces-fill_regions-%d.svg", iRun ++).c_str(), get_extents(fill_boundaries_ex));
            svg.draw(fill_boundaries_ex);
            svg.draw_outline(fill_boundaries_ex, "black", "blue", scale_(0.05)); 
            svg.Close();
        }

//        export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
 
        {
            // Bridge expolygons, grown, to be tested for intersection with other bridge regions.
            std::vector<BoundingBox> fill_boundaries_ex_bboxes = get_extents_vector(fill_boundaries_ex);
            bridges_grown.reserve(bridges.size());
            bridge_bboxes.reserve(bridges.size());
            for (size_t i = 0; i < bridges.size(); ++ i) {
                // Find the island of this bridge.
                const Point pt = bridges[i].expolygon.contour.points.front();
                int idx_island = -1;
                for (int j = 0; j < int(fill_boundaries_ex.size()); ++ j)
                    if (fill_boundaries_ex_bboxes[j].contains(pt) && 
                        fill_boundaries_ex[j].contains(pt)) {
                        idx_island = j;
                        break;
                    }
                // Grown by bridged_infill_margin.
                // also, remove all bridge area that are thinner than a single line.
                Polygons polys = offset2(to_polygons(bridges[i].expolygon),
                    (-this->flow(frInfill).scaled_width() / 2), 
                    (this->flow(frInfill).scaled_width() / 2) + float(margin_bridged), 
                    EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                if (idx_island == -1) {
                    BOOST_LOG_TRIVIAL(trace) << "Bridge did not fall into the source region!";
                } else {
                    // Found an island, to which this bridge region belongs. Trim it,
                    polys = intersection(polys, to_polygons(fill_boundaries_ex[idx_island]));
                }
                bridge_bboxes.push_back(get_extents(polys));
                bridges_grown.push_back(std::move(polys));
            }
        }

        // 2) Group the bridge surfaces by overlaps.
        std::vector<size_t> bridge_group(bridges.size(), (size_t)-1);
        size_t n_groups = 0; 
        for (size_t i = 0; i < bridges.size(); ++ i) {
            // A grup id for this bridge.
            size_t group_id = (bridge_group[i] == size_t(-1)) ? (n_groups ++) : bridge_group[i];
            bridge_group[i] = group_id;
            // For all possibly overlaping bridges:
            for (size_t j = i + 1; j < bridges.size(); ++ j) {
                if (! bridge_bboxes[i].overlap(bridge_bboxes[j]))
                    continue;
                if (intersection(bridges_grown[i], bridges_grown[j], false).empty())
                    continue;
                // The two bridge regions intersect. Give them the same group id.
                if (bridge_group[j] != size_t(-1)) {
                    // The j'th bridge has been merged with some other bridge before.
                    size_t group_id_new = bridge_group[j];
                    for (size_t k = 0; k < j; ++ k)
                        if (bridge_group[k] == group_id)
                            bridge_group[k] = group_id_new;
                    group_id = group_id_new;
                }
                bridge_group[j] = group_id;
            }
        }

        // 3) Merge the groups with the same group id, detect bridges.
        {
            BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z << ", bridge groups: " << n_groups;
            for (size_t group_id = 0; group_id < n_groups; ++ group_id) {
                size_t n_bridges_merged = 0;
                size_t idx_last = (size_t)-1;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] == group_id) {
                        ++ n_bridges_merged;
                        idx_last = i;
                    }
                }
                if (n_bridges_merged == 0)
                    // This group has no regions assigned as these were moved into another group.
                    continue;
                // Collect the initial ungrown regions and the grown polygons.
                ExPolygons  initial;
                Polygons    grown;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] != group_id)
                        continue;
                    initial.push_back(std::move(bridges[i].expolygon));
                    polygons_append(grown, bridges_grown[i]);
                }
                // detect bridge direction before merging grown surfaces otherwise adjacent bridges
                // would get merged into a single one while they need different directions
                // also, supply the original expolygon instead of the grown one, because in case
                // of very thin (but still working) anchors, the grown expolygon would go beyond them
                BridgeDetector bd(
                    initial,
                    lower_layer->lslices,
                    this->flow(frInfill, true).scaled_width()
                );
                #ifdef SLIC3R_DEBUG
                printf("Processing bridge at layer %zu:\n", this->layer()->id());
                #endif
                double custom_angle = Geometry::deg2rad(this->region()->config().bridge_angle.value);
                if (custom_angle > 0) {
                    // Bridge was not detected (likely it is only supported at one side). Still it is a surface filled in
                    // using a bridging flow, therefore it makes sense to respect the custom bridging direction.
                    bridges[idx_last].bridge_angle = custom_angle;
                }else if (bd.detect_angle(custom_angle)) {
                    bridges[idx_last].bridge_angle = bd.angle;
                    if (this->layer()->object()->config().support_material) {
                        //polygons_append(this->bridged, intersection(bd.coverage(), to_polygons(initial)));
                        append(this->unsupported_bridge_edges, bd.unsupported_edges());
                    }
                } else {
                    bridges[idx_last].bridge_angle = 0;
                }
                // without safety offset, artifacts are generated (GH #2494)
                surfaces_append(bottom, union_ex(grown, true), bridges[idx_last]);
            }

            fill_boundaries = std::move(to_polygons(fill_boundaries_ex));
            BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
        }

    #if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun ++), true);
        }
    #endif
    }

    Surfaces new_surfaces;
    {
        // Merge top and bottom in a single collection.
        surfaces_append(top, std::move(bottom));
        // Intersect the grown surfaces with the actual fill boundaries.
        Polygons bottom_polygons = to_polygons(bottom);
        for (size_t i = 0; i < top.size(); ++ i) {
            Surface &s1 = top[i];
            if (s1.empty())
                continue;
            Polygons polys;
            polygons_append(polys, std::move(s1));
            for (size_t j = i + 1; j < top.size(); ++ j) {
                Surface &s2 = top[j];
                if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                    polygons_append(polys, std::move(s2));
                    s2.clear();
                }
            }
            if (s1.has_pos_top())
                // Trim the top surfaces by the bottom surfaces. This gives the priority to the bottom surfaces.
                polys = diff(polys, bottom_polygons);
            surfaces_append(
                new_surfaces,
                // Don't use a safety offset as fill_boundaries were already united using the safety offset.
                std::move(intersection_ex(polys, fill_boundaries, false)),
                s1);
        }
    }
    
    // Subtract the new top surfaces from the other non-top surfaces and re-add them.
    Polygons new_polygons = to_polygons(new_surfaces);
    for (size_t i = 0; i < internal.size(); ++ i) {
        Surface &s1 = internal[i];
        if (s1.empty())
            continue;
        Polygons polys;
        polygons_append(polys, std::move(s1));
        for (size_t j = i + 1; j < internal.size(); ++ j) {
            Surface &s2 = internal[j];
            if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                polygons_append(polys, std::move(s2));
                s2.clear();
            }
        }
        ExPolygons new_expolys = diff_ex(polys, new_polygons);
        polygons_append(new_polygons, to_polygons(new_expolys));
        surfaces_append(new_surfaces, std::move(new_expolys), s1);
    }
    
    this->fill_surfaces.surfaces = std::move(new_surfaces);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */ 

    /*  Note: in order to make the psPrepareInfill step idempotent, we should never
        alter fill_surfaces boundaries on which our idempotency relies since that's
        the only meaningful information returned by psPerimeters. */
    
    bool spiral_vase = this->layer()->object()->print()->config().spiral_vase;

    // if no solid layers are requested, turn top/bottom surfaces to internal
    if (! spiral_vase && this->region()->config().top_solid_layers == 0) {
        for (Surfaces::iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
            if (surface->has_pos_top())
                surface->surface_type = (this->layer()->object()->config().infill_only_where_needed) ? 
                    stPosInternal | stDensVoid : stPosInternal | stDensSparse;
    }
    if (this->region()->config().bottom_solid_layers == 0) {
        for (Surfaces::iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface) {
            if (surface->has_pos_bottom())
                surface->surface_type = stPosInternal | stDensSparse;
        }
    }
        
    // turn too small internal regions into solid regions according to the user setting
    if (! spiral_vase && this->region()->config().fill_density.value > 0) {
        // scaling an area requires two calls!
        double min_area = scale_(scale_(this->region()->config().solid_infill_below_area.value));
        for (Surfaces::iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface) {
            if (surface->has_fill_sparse() && surface->has_pos_internal() && surface->area() <= min_area)
                surface->surface_type = stPosInternal | stDensSolid;
        }
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices().surfaces)
        assert(surface.surface_type == (stPosInternal | stDensSparse));
#endif /* NDEBUG */
    this->m_slices.set(intersection_ex(to_polygons(std::move(this->slices().surfaces)), trimming_polygons, false), stPosInternal | stDensSparse);
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices().surfaces)
        assert(surface.surface_type == (stPosInternal | stDensSparse));
#endif /* NDEBUG */
    ExPolygons slices_expolygons = to_expolygons(std::move(this->slices().surfaces));
    Polygons   slices_polygons   = to_polygons(slices_expolygons);
    Polygons   tmp               = intersection(slices_polygons, trimming_polygons, false);
    append(tmp, diff(slices_polygons, offset(offset_ex(slices_expolygons, -elephant_foot_compensation_perimeter_step), elephant_foot_compensation_perimeter_step)));
    this->m_slices.set(std::move(union_ex(tmp)), stPosInternal | stDensSparse);
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (const Surface& surface : this->slices().surfaces)
        bbox.merge(get_extents(surface.expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (Surfaces::const_iterator surface = this->slices().surfaces.begin(); surface != this->slices().surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        svg.draw(surface->expolygon.lines(), surface_type_to_color_name(surface->surface_type));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces.surfaces) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05)); 
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

}
 
