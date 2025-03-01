/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "BKE_node.h"

extern struct bNodeTreeType *ntreeType_Geometry;

void register_node_tree_type_geo(void);

void register_node_type_geo_group(void);
void register_node_type_geo_custom_group(bNodeType *ntype);

void register_node_type_geo_align_rotation_to_vector(void);
void register_node_type_geo_attribute_clamp(void);
void register_node_type_geo_attribute_color_ramp(void);
void register_node_type_geo_attribute_combine_xyz(void);
void register_node_type_geo_attribute_compare(void);
void register_node_type_geo_attribute_convert(void);
void register_node_type_geo_attribute_curve_map(void);
void register_node_type_geo_attribute_fill(void);
void register_node_type_geo_attribute_map_range(void);
void register_node_type_geo_attribute_math(void);
void register_node_type_geo_attribute_mix(void);
void register_node_type_geo_attribute_proximity(void);
void register_node_type_geo_attribute_randomize(void);
void register_node_type_geo_attribute_separate_xyz(void);
void register_node_type_geo_attribute_transfer(void);
void register_node_type_geo_attribute_vector_math(void);
void register_node_type_geo_attribute_vector_rotate(void);
void register_node_type_geo_attribute_remove(void);
void register_node_type_geo_boolean(void);
void register_node_type_geo_bounding_box(void);
void register_node_type_geo_collection_info(void);
void register_node_type_geo_convex_hull(void);
void register_node_type_geo_curve_length(void);
void register_node_type_geo_curve_to_mesh(void);
void register_node_type_geo_curve_to_points(void);
void register_node_type_geo_curve_resample(void);
void register_node_type_geo_curve_reverse(void);
void register_node_type_geo_delete_geometry(void);
void register_node_type_geo_edge_split(void);
void register_node_type_geo_input_material(void);
void register_node_type_geo_is_viewport(void);
void register_node_type_geo_join_geometry(void);
void register_node_type_geo_material_assign(void);
void register_node_type_geo_material_replace(void);
void register_node_type_geo_mesh_primitive_circle(void);
void register_node_type_geo_mesh_primitive_cone(void);
void register_node_type_geo_mesh_primitive_cube(void);
void register_node_type_geo_mesh_primitive_cylinder(void);
void register_node_type_geo_mesh_primitive_grid(void);
void register_node_type_geo_mesh_primitive_ico_sphere(void);
void register_node_type_geo_mesh_primitive_line(void);
void register_node_type_geo_mesh_primitive_uv_sphere(void);
void register_node_type_geo_mesh_to_curve(void);
void register_node_type_geo_object_info(void);
void register_node_type_geo_point_distribute(void);
void register_node_type_geo_point_instance(void);
void register_node_type_geo_point_rotate(void);
void register_node_type_geo_point_scale(void);
void register_node_type_geo_point_separate(void);
void register_node_type_geo_point_translate(void);
void register_node_type_geo_points_to_volume(void);
void register_node_type_geo_sample_texture(void);
void register_node_type_geo_select_by_material(void);
void register_node_type_geo_separate_components(void);
void register_node_type_geo_subdivide(void);
void register_node_type_geo_subdivision_surface(void);
void register_node_type_geo_switch(void);
void register_node_type_geo_transform(void);
void register_node_type_geo_triangulate(void);
void register_node_type_geo_volume_to_mesh(void);

#ifdef __cplusplus
}
#endif
