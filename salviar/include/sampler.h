#ifndef SALVIAR_SAMPLER_H
#define SALVIAR_SAMPLER_H

#include "../include/colors.h"
#include "../include/enums.h"

#include <eflib/include/platform/typedefs.h>
#include <salviar/include/salviar_forward.h>
#include "texture.h"
BEGIN_NS_SALVIAR()


class texture_1d;
class texture_2d;
class surface;

struct sampler_desc {
    filter_type min_filter;
	filter_type mag_filter;
	filter_type mip_filter;
    address_mode addr_mode_u;
    address_mode addr_mode_v;
    address_mode addr_mode_w;
    float mip_lod_bias;
    uint32_t max_anisotropy;
    compare_function comparison_func;
    color_rgba32f border_color;
    float min_lod;
    float max_lod;

	sampler_desc():
		min_filter(filter_point),
		mag_filter(filter_point),
		mip_filter(filter_point),
		addr_mode_u(address_wrap),
		addr_mode_v(address_wrap),
		addr_mode_w(address_wrap),
		mip_lod_bias(0),
		max_anisotropy(0),
		comparison_func(compare_function_always),
		border_color(color_rgba32f(0.0f, 0.0f, 0.0f, 0.0f)),
		min_lod(-1e20f),
		max_lod(1e20f)
	{
	}
};

class sampler
{
public:
	typedef color_rgba32f (*filter_op_type)(const surface& surf, float x, float y, size_t sample, const color_rgba32f& border_color);

private:
	sampler_desc desc_;
	const texture* ptex_;
	filter_op_type filters_[sampler_state_count];

	float calc_lod( eflib::int4 const& size, eflib::vec4 const& ddx, eflib::vec4 const& ddy, float bias ) const;
	float calc_lod(
		const eflib::vec4& unproj_attr, 
		const eflib::int4& size, 
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy, 
		float inv_x_w, float inv_y_w, float inv_w, float bias) const;

	void calc_anisotropic_lod(
		eflib::int4 const& size,
		eflib::vec4 const& ddx, eflib::vec4 const& ddy, float bias,
		float& out_lod, float& out_ratio, eflib::vec4& out_long_axis ) const;

	void calc_anisotropic_lod(
		const eflib::vec4& unproj_attr, 
		const eflib::int4& size, 
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy, 
		float inv_x_w, float inv_y_w, float inv_w, float bias,
		float& out_lod, float& out_ratio, eflib::vec4& out_long_axis
		) const;

	color_rgba32f sample_surface(
		const surface& surf,
		float x, float y, size_t sample,
		sampler_state ss) const;

	color_rgba32f sample_impl(
		const texture *tex ,
		float coordx, float coordy,
		size_t sample, float miplevel,
		float ratio, eflib::vec4 const& long_axis) const;

	color_rgba32f sample_impl(const texture *tex , 
		float coordx, float coordy, size_t sample, 
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy, 
		float inv_x_w, float inv_y_w, float inv_w, float lod_bias) const;

	color_rgba32f sample_2d_impl(const texture *tex , 
		const eflib::vec4& coord, size_t sample,
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy,
		float inv_x_w, float inv_y_w, float inv_w, float lod_bias) const;

public:
	explicit sampler(const sampler_desc& desc);

	void set_texture(const texture* ptex){ptex_ = ptex;}
	void set_sampler_desc( sampler_desc const& desc );

	color_rgba32f sample(float coordx, float coordy, float miplevel) const;

	color_rgba32f sample(
		float coordx, float coordy, 
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy, 
		float inv_x_w, float inv_y_w, float inv_w, float lod_bias) const;

	color_rgba32f sample_2d(
		const eflib::vec4& proj_coord,
		const eflib::vec4& unproj_ddx, const eflib::vec4& unproj_ddy,
		float inv_x_w, float inv_y_w, float inv_w, float lod_bias) const;

	color_rgba32f sample_2d_lod( eflib::vec2 const& proj_coord, float lod ) const;

	color_rgba32f sample_2d_grad(
		eflib::vec2 const& proj_coord,
		eflib::vec2 const& ddx, eflib::vec2 const& ddy, float lod_bias
		) const;

	color_rgba32f sample_2d_proj(
		eflib::vec4 const& proj_coord,
		eflib::vec4 const& ddx, eflib::vec4 const& ddy 
		) const;

	color_rgba32f sample_cube(
		float coordx, float coordy, float coordz,
		float miplevel
		) const;

	color_rgba32f sample_cube(
		const eflib::vec4& coord,
		const eflib::vec4& unproj_ddx,
		const eflib::vec4& unproj_ddy,
		float inv_x_w, float inv_y_w, float inv_w, float lod_bias
		) const;
};


END_NS_SALVIAR()
#endif
