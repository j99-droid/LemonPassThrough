/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.

#include "compositor.h"

// Monado includes
#include "driver/xrt_cast.h"
#include "main/comp_frame.h"
#include "util/comp_render_helpers.h"
#include "util/comp_vulkan.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_time.h"
#include "vk/vk_helpers.h"

#include "driver/wivrn_session.h"
#include "encoder/video_encoder.h"
#include "inplace_vector.hpp"
#include "utils/method.h"
#include "utils/wivrn_trace.h"

#include "xrt/xrt_config_build.h" // IWYU pragma: keep

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#ifdef XRT_FEATURE_RENDERDOC
#include "renderdoc_app.h"

static auto renderdoc()
{
	auto x = []() {
		RENDERDOC_API_1_5_0 * rdoc_api = nullptr;
		const char * env = std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
		if (not env or env != std::string_view("1"))
			return rdoc_api;
		void * mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
		if (mod)
		{
			pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
			XRT_MAYBE_UNUSED int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_5_0, (void **)&rdoc_api);
			assert(ret == 1);
		}
		return rdoc_api;
	};
	static auto res = x();
	return res;
}
#endif

DEBUG_GET_ONCE_LOG_OPTION(log, "XRT_COMPOSITOR_LOG", U_LOGGING_INFO)

namespace details
{
template <auto Method, typename Result, typename... Args>
struct method_trait<Method, Result (wivrn::compositor::*)(Args...)>
{
	static Result magic(xrt_compositor * arg, Args... args)
	{
		return std::invoke(
		        Method,
		        static_cast<wivrn::compositor *>(reinterpret_cast<struct comp_base *>(arg)),
		        args...);
	}
};

} // namespace details

namespace
{
const comp_swapchain_image & get_layer_image(const comp_layer & layer, uint32_t swapchain_index, uint32_t image_index)
{
	return reinterpret_cast<struct comp_swapchain *>(comp_layer_get_swapchain(&layer, swapchain_index))->images[image_index];
}

std::array<vk::Format, 3> image_formats(int bit_depth)
{
	switch (bit_depth)
	{
		case 8:
			return {
			        vk::Format::eR8Unorm,
			        vk::Format::eR8G8Unorm,
			        vk::Format::eG8B8R82Plane420Unorm,
			};
		case 10:
			return {
			        vk::Format::eR16Unorm,
			        vk::Format::eR16G16Unorm,
			        vk::Format::eG10X6B10X6R10X62Plane420Unorm3Pack16,
			};
	}
	throw std::runtime_error(std::format("Unsupported bit depth {}", bit_depth));
}

std::array<wivrn::compositor::image, 2> make_images(wivrn::vk_bundle & vk, vk::CommandPool command_pool, std::span<wivrn::encoder_settings> encoders)
{
	auto formats = image_formats(encoders[0].bit_depth);

	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {
	                        .width = encoders[0].width,
	                        .height = encoders[0].height,
	                        .depth = 1,
	                },
	                .mipLevels = 1,
	                .arrayLayers = 3, // left, right then alpha
	                .samples = vk::SampleCountFlagBits::e1,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
#if WIVRN_USE_VULKAN_ENCODE
	if (
	        std::get<vk::PhysicalDeviceVideoMaintenance1FeaturesKHR>(vk.feat).videoMaintenance1 and
	        std::ranges::contains(
	                encoders,
	                wivrn::encoder_vulkan,
	                &wivrn::encoder_settings::encoder_name))
	{
		image_info.get().flags |= vk::ImageCreateFlagBits::eVideoProfileIndependentKHR;
		image_info.get().usage |= vk::ImageUsageFlagBits::eVideoEncodeSrcKHR;
	}
#endif

	auto make_image = [&](int i) {
		vk::ImageViewUsageCreateInfo usage{
		        .usage = vk::ImageUsageFlagBits::eStorage,
		};
		image_allocation image{
		        vk.device,
		        image_info.get(),
		        VmaAllocationCreateInfo{.usage = VMA_MEMORY_USAGE_AUTO},
		        std::format("compositor YCbCr image {}", i),
		};
		vk::Image vk_image{image};
		return wivrn::compositor::image{
		        .image{std::move(image)},
		        .view_y{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[0],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane0,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        },
		        .view_cbcr{
		                vk.device,
		                {
		                        .pNext = &usage,
		                        .image = vk_image,
		                        .viewType = vk::ImageViewType::e2DArray,
		                        .format = formats[1],
		                        .subresourceRange = {
		                                .aspectMask = vk::ImageAspectFlagBits::ePlane1,
		                                .levelCount = 1,
		                                .layerCount = image_info.get().arrayLayers,
		                        },
		                },
		        }};
	};

	return {make_image(0), make_image(1)};
}

vk::raii::Semaphore make_semaphore(wivrn::vk_bundle & vk)
{
	vk::raii::Semaphore res{
	        vk.device,
	        vk::StructureChain{
	                vk::SemaphoreCreateInfo{},
	                vk::SemaphoreTypeCreateInfo{
	                        .semaphoreType = vk::SemaphoreType::eTimeline,
	                },
	        }
	                .get(),
	};
	vk.name(res, "compositor semaphore");
	return res;
}

vk::Extent3D render_extent(const wivrn::from_headset::headset_info_packet & info)
{
	return {
	        .width = info.render_eye_width,
	        .height = info.render_eye_height,
	        .depth = 1,
	};
}

} // namespace

namespace wivrn
{

void compositor::timings::add(float us)
{
	int index = this->index;
	this->index = (this->index + 1) % values.size();
	values[index] = us;
}

xrt_result_t compositor::predict_frame(int64_t * out_frame_id,
                                       int64_t * out_wake_time_ns,
                                       int64_t * out_predicted_gpu_time_ns,
                                       int64_t * out_predicted_display_time_ns,
                                       int64_t * out_predicted_display_period_ns)
{
	int64_t frame_id = -1;
	int64_t wake_up_time_ns = 0;
	int64_t present_slop_ns = 0;
	int64_t desired_present_time_ns = 0;
	int64_t predicted_display_time_ns = 0;
	pacer.predict(
	        frame_id,
	        wake_up_time_ns,
	        desired_present_time_ns,
	        present_slop_ns,
	        predicted_display_time_ns);

	frame.waited.id = frame_id;
	frame.waited.desired_present_time_ns = desired_present_time_ns;
	frame.waited.present_slop_ns = present_slop_ns;
	frame.waited.predicted_display_time_ns = predicted_display_time_ns;

	*out_frame_id = frame_id;
	*out_wake_time_ns = wake_up_time_ns;
	*out_predicted_gpu_time_ns = desired_present_time_ns; // Not quite right but close enough.
	*out_predicted_display_time_ns = predicted_display_time_ns;
	*out_predicted_display_period_ns = pacer.get_frame_duration();
	return XRT_SUCCESS;
}

xrt_result_t compositor::mark_frame(int64_t frame_id,
                                    xrt_compositor_frame_point point,
                                    int64_t when_ns)
{
	switch (point)
	{
		case XRT_COMPOSITOR_FRAME_POINT_WOKE:
			trace::instant_feedback("wake_up", when_ns, frame_id);
			return XRT_SUCCESS;
		default:
			assert(false);
	}
	return XRT_ERROR_VULKAN;
}

xrt_result_t compositor::layer_commit(xrt_graphics_sync_handle_t sync_handle)
{
	u_graphics_sync_unref(&sync_handle);

	// Move waited frame to rendering frame, clear waited.
	comp_frame_move_and_clear_locked(&frame.rendering, &frame.waited);

	U_LOG_IFL_D(log_level, "frame %ld commit %d layers", frame.rendering.id, layer_accum.layer_count);

	if (encode_request >= 0 // encoders have not picked up the previous frame
	    or not session.connected() or not session.get_offset())
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

	int i = acquire_image();
	if (i < 0)
	{
		comp_frame_clear_locked(&frame.rendering);
		return XRT_SUCCESS;
	}

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->StartFrameCapture(NULL, NULL);
#endif

	// The game submits opaque frames, so force alpha blend while the mask is on.
	// The headset then composites passthrough behind the transparent rectangle.
	// Must happen before view_info.alpha is read below.
	update_wheel_mask();
	if (wheel.enabled)
		layer_accum.data.env_blend_mode = XRT_BLEND_MODE_ALPHA_BLEND;

	auto & view_info = images[i].view_info;
	images[i].frame_index = frame.rendering.id;
	view_info = {
	        .display_time = session.get_offset().to_headset(frame.rendering.predicted_display_time_ns),
	        .alpha = layer_accum.data.env_blend_mode == XRT_BLEND_MODE_ALPHA_BLEND,
	};

	trace::instant_feedback("begin", frame.rendering.id, os_monotonic_get_ns());

	cmd_pool.reset();
	cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

	cmd.resetQueryPool(*query_pool, 0, 3);
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *query_pool, 0);

	bool flip_y = false;
	std::array<vk::ImageView, 2> src;
	std::array<xrt_rect, 2> src_rect;
	std::array<xrt_fov, 2> src_fov;

	beman::inplace_vector::inplace_vector<vk::ImageMemoryBarrier2, 3> image_barriers;

	// Check if we can pass a layer directly to foveation
	if (layer_accum.layer_count == 1 and
	    (layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION or
	     layer_accum.layers[0].data.type == XRT_LAYER_PROJECTION_DEPTH))
	{
		const auto & layer = layer_accum.layers[0];
		for (int view = 0; view < 2; ++view)
		{
			const auto & data = (layer.data.type == XRT_LAYER_PROJECTION ? layer.data.proj.v : layer.data.depth.v)[view];
			src[view] = get_image_view(
			        &get_layer_image(layer, view, data.sub.image_index),
			        layer.data.flags,
			        data.sub.array_index);
			src_rect[view] = data.sub.rect;
			src_fov[view] = data.fov;
			flip_y = layer.data.flip_y;
			view_info.pose[view] = xrt_cast(data.pose);
			view_info.fov[view] = xrt_cast(data.fov);
		}
	}
	else
	{
		// no fast-path, squash layers
		std::array<xrt_pose, 2> poses;
		const auto extent = images[0].image.info().extent;
		std::tie(poses, src_fov, src_rect) = squasher.do_layers(
		        vk.device,
		        cmd,
		        session.get_hmd(),
		        pacer.get_frame_duration(),
		        frame.rendering,
		        layer_accum,
		        xrt_rect{.extent{.w = int(extent.width), .h = int(extent.height)}});

		src = squasher.get_views();

		for (int view = 0; view < 2; ++view)
		{
			view_info.pose[view] = xrt_cast(poses[view]);
			view_info.fov[view] = xrt_cast(src_fov[view]);
		}

		image_barriers.push_back(
		        vk::ImageMemoryBarrier2{
		                .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
		                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
		                .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
		                .oldLayout = vk::ImageLayout::eGeneral,
		                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
		                .image = squasher.get_image(),
		                .subresourceRange = {
		                        .aspectMask = vk::ImageAspectFlagBits::eColor,
		                        .levelCount = 1,
		                        .layerCount = 2,
		                },
		        });
	}

	image_barriers.push_back(
	        vk::ImageMemoryBarrier2{
	                .dstStageMask = vk::PipelineStageFlagBits2::eComputeShader,
	                .dstAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
	                .oldLayout = vk::ImageLayout::eUndefined,
	                .newLayout = vk::ImageLayout::eGeneral,
	                .image = images[i].image,
	                .subresourceRange = {
	                        .aspectMask = vk::ImageAspectFlagBits::eColor,
	                        .levelCount = 1,
	                        .layerCount = images[i].image.info().arrayLayers,
	                },
	        });

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	image_barriers.clear();

	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 1);

	apply_wheel_mask(view_info.pose, view_info.fov, flip_y);

	view_info.foveation = foveation.foveate(
	        vk.device,
	        cmd,
	        images[i].view_y,
	        images[i].view_cbcr,
	        flip_y,
	        src,
	        src_rect,
	        src_fov,
	        view_info.alpha);

	for (auto & encoder: encoders)
	{
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		else if (encoder->need_transfer or encoder->target_queue == vk.queue.family_index)
		{
			image_barriers.push_back(
			        vk::ImageMemoryBarrier2{
			                .srcStageMask = vk::PipelineStageFlagBits2KHR::eComputeShader,
			                .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
			                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
			                .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
			                // For a queue-family ownership transfer in EXCLUSIVE
			                // sharing mode, the release barrier's old/new layout
			                // must match the encoder-side acquire. newLayout is
			                // the encoder's target_layout; per the VkImageMemoryBarrier2
			                // spec the layout transition is executed exactly once
			                // between the queues, so this single QFOT barrier covers
			                // both the queue-family transfer and the layout
			                // transition the encoder needs.
			                .oldLayout = vk::ImageLayout::eGeneral,
			                .newLayout = encoder->target_layout,
			                .srcQueueFamilyIndex = vk.queue.family_index,
			                .dstQueueFamilyIndex = encoder->target_queue,
			                .image = images[i].image,
			                .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
			                                     .baseMipLevel = 0,
			                                     .levelCount = 1,
			                                     .baseArrayLayer = encoder->stream_idx,
			                                     .layerCount = 1},
			        });
		}
	}

	cmd.pipelineBarrier2({
	        .imageMemoryBarrierCount = uint32_t(image_barriers.size()),
	        .pImageMemoryBarriers = image_barriers.data(),
	});
	cmd.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *query_pool, 2);

	cmd.end();

	const vk::SemaphoreSubmitInfo sem_info{
	        .semaphore = *sem,
	        .value = ++sem_value,
	        .stageMask = vk::PipelineStageFlagBits2::eComputeShader,
	};

	{
		vk::CommandBufferSubmitInfo cmd_info{
		        .commandBuffer = cmd,
		};
		std::unique_lock lock{vk.queue.mutex};
		vk.queue.queue.submit2(vk::SubmitInfo2{
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmd_info,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &sem_info,
		});
	}

	pacer.mark_timing_point(COMP_TARGET_TIMING_POINT_SUBMIT_END, frame.rendering.id, os_monotonic_get_ns());
	auto info = pacer.present_to_info(frame.rendering.desired_present_time_ns);

	for (auto & encoder: encoders)
	{
		if (encoder->stream_idx == 2 and not view_info.alpha)
			continue;
		encoder->present_image(
		        images[i].image,
		        sem_info,
		        info.frame_id);
	}

	[[maybe_unused]] auto j = encode_request.exchange(i);
	encode_request.notify_all();
	assert(j == -1);

#ifdef XRT_FEATURE_RENDERDOC
	if (auto r = renderdoc())
		r->EndFrameCapture(NULL, NULL);
#endif

	comp_frame_clear_locked(&frame.rendering);

	if (vk.device.waitSemaphores(vk::SemaphoreWaitInfo{
	                                     .semaphoreCount = 1,
	                                     .pSemaphores = &*sem,
	                                     .pValues = &sem_info.value,
	                             },
	                             U_TIME_1S_IN_NS) == vk::Result::eTimeout)
	{
		U_LOG_IFL_W(log_level, "compositor timeout");
	}
	else
	{
		auto [res, ts] = query_pool.getResults<uint64_t>(
		        0,
		        3,
		        3 * sizeof(uint64_t),
		        sizeof(uint64_t),
		        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);

		if (res == vk::Result::eSuccess)
		{
			static const auto period = vk.physical_device.getProperties().limits.timestampPeriod;
			squasher_times.add((ts[1] - ts[0]) * period / 1e3);
			foveation_times.add((ts[2] - ts[1]) * period / 1e3);
		}
	}

	// Now is a good point to garbage collect.
	comp_swapchain_shared_garbage_collect(&cscs);

	return XRT_SUCCESS;
}

xrt_result_t compositor::get_display_refresh_rate(float * hz)
{
	auto settings = session.get_settings();
	// there should not be rounding errors, hz must be one of the available refresh rates
	*hz = frame_rate * settings->fps_divider;
	assert(std::ranges::contains(session.get_info().available_refresh_rates, *hz));
	return XRT_SUCCESS;
}

xrt_result_t compositor::request_display_refresh_rate(float hz)
{
	requested_refresh_rate = hz;
	U_LOG_I("request refresh rate: %fHz", hz);
	if (hz > 0)
	{
		try
		{
			session.send_control(to_headset::refresh_rate_change{.hz = hz});
		}
		catch (std::exception & e)
		{
			U_LOG_W("refresh rate change failed: %s", e.what());
		}
	}
	return XRT_SUCCESS;
}

xrt_result_t compositor::get_view_config(
        xrt_compositor_native * self,
        xrt_view_type view_type,
        xrt_view_config * out_view_config)
{
	const auto & session = reinterpret_cast<compositor *>(self)->session;
	const auto extent = render_extent(session.get_info());
	switch (view_type)
	{
		case XRT_VIEW_TYPE_MONO:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
		case XRT_VIEW_TYPE_STEREO:
			*out_view_config = {
			        .view_type = XRT_VIEW_TYPE_STEREO,
			        .view_count = 2,
			        .views = {}};
			for (auto & view: std::span(out_view_config->views, out_view_config->view_count))
			{
				view = {
				        .recommended = {
				                .width_pixels = extent.width,
				                .height_pixels = extent.height,
				                .sample_count = 1,
				        },
				        .max = {
				                .width_pixels = extent.width * 2u,
				                .height_pixels = extent.height * 2u,
				                .sample_count = 1,
				        },
				};
			}
			return XRT_SUCCESS;
		case XRT_VIEW_TYPE_QUAD:
		case XRT_VIEW_TYPE_INVALID:
		case XRT_VIEW_TYPE_MAX:
			return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
	}
	return XRT_ERROR_UNSUPPORTED_VIEW_TYPE;
}

int compositor::acquire_image()
{
	for (auto [i, image]: std::ranges::enumerate_view(images))
	{
		if (not image.busy.exchange(true))
			return i;
	}
	return -1;
}

void compositor::encoder_work(std::stop_token tok)
{
	while (not tok.stop_requested())
	{
		auto req = encode_request.exchange(-1);
		if (req < 0)
		{
			encode_request.wait(req);
			wivrn::trace::cpu_instant(wivrn::trace::cpu_track::compositor, "encoder_work wake", 0, 0);
			continue;
		}

		assert(req < images.size());
		auto & image = images[req];

		wivrn::trace::scope trace_iter(wivrn::trace::cpu_track::compositor, 0, image.frame_index, "encoder_work iter");

		try
		{
			for (auto & encoder: encoders)
			{
				if (encoder->stream_idx < 2 or image.view_info.alpha)
					encoder->encode(session, image.view_info, image.frame_index);
			}
		}
		catch (std::exception & e)
		{
			U_LOG_W("encode error: %s", e.what());
		}
		image.busy = false;
	}
}

void compositor::send_video_stream_description()

{
	to_headset::video_stream_description desc{
	        .width = uint16_t(images[0].image.info().extent.width),
	        .height = uint16_t(images[0].image.info().extent.height),
	        .frame_rate = settings[0].fps,
	};
	get_display_refresh_rate(&desc.refresh_rate);
	static_assert(std::tuple_size_v<decltype(settings)> == std::tuple_size_v<decltype(desc.codec)>);
	std::ranges::transform(settings, desc.codec.begin(), &encoder_settings::codec);
	session.send_control(std::move(desc));
}

namespace
{
// Wheel placement and mask editing. Head-space rectangles are in eye UV (0..1, top-left origin).
constexpr float wheel_min_uv_size = 0.02f;   // smallest head-space width or height
constexpr float wheel_min_metres = 0.1f;     // smallest wheel width or height
constexpr float wheel_move_speed = 0.004f;   // UV per frame at full left-stick deflection
constexpr float wheel_nudge_speed = 0.002f;  // metres per frame at full left-stick deflection
constexpr float wheel_size_speed = 0.002f;   // metres per frame at full right-stick deflection
constexpr float wheel_deadzone = 0.15f;
// A newly placed wheel goes below and in front of the head, in the head's orientation.
constexpr XrVector3f wheel_place_offset{0.0f, -0.25f, -0.5f};

XrVector3f add(XrVector3f a, XrVector3f b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

XrVector3f cross(XrVector3f a, XrVector3f b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Rotates v by the unit quaternion q.
XrVector3f rotate(XrQuaternionf q, XrVector3f v)
{
	XrVector3f u{q.x, q.y, q.z};
	XrVector3f t = cross(u, v);
	XrVector3f tt = cross(u, t);
	return {v.x + 2 * (q.w * t.x + tt.x), v.y + 2 * (q.w * t.y + tt.y), v.z + 2 * (q.w * t.z + tt.z)};
}

// Keeps only the yaw of q, so a box placed with it stands upright whatever the head's pitch and roll.
XrQuaternionf yaw_only(XrQuaternionf q)
{
	XrVector3f forward = rotate(q, {0, 0, -1});
	float yaw = std::atan2(-forward.x, -forward.z);
	return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
}

// Moves a tracking-space point into a view's space, where the view looks down -Z.
XrVector3f to_view(const XrPosef & view, XrVector3f world)
{
	XrVector3f rel{world.x - view.position.x, world.y - view.position.y, world.z - view.position.z};
	XrQuaternionf inverse{-view.orientation.x, -view.orientation.y, -view.orientation.z, view.orientation.w};
	return rotate(inverse, rel);
}

// Projects a view-space point in front of the eye into the eye's UV.
// The eye image is a flat projection, so UV is linear in the tangent of the angle, not the angle.
XrVector2f project(const XrFovf & fov, bool flip_y, XrVector3f local)
{
	float tx = local.x / -local.z;
	float ty = local.y / -local.z;
	float left = std::tan(fov.angleLeft);
	float right = std::tan(fov.angleRight);
	float up = std::tan(fov.angleUp);
	float down = std::tan(fov.angleDown);
	XrVector2f uv{(tx - left) / (right - left), (up - ty) / (up - down)};
	if (flip_y)
		uv.y = 1 - uv.y;
	return uv;
}

// Clips a view-space polygon to the part in front of the eye's near plane.
std::vector<XrVector3f> clip_near(const std::vector<XrVector3f> & polygon)
{
	constexpr float near = 0.01f;
	auto inside = [](XrVector3f p) { return p.z <= -near; };
	std::vector<XrVector3f> out;
	for (size_t i = 0; i < polygon.size(); ++i)
	{
		XrVector3f a = polygon[i];
		XrVector3f b = polygon[(i + 1) % polygon.size()];
		if (inside(a))
			out.push_back(a);
		if (inside(a) != inside(b))
		{
			float t = (-near - a.z) / (b.z - a.z);
			out.push_back({a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), -near});
		}
	}
	return out;
}

std::filesystem::path wheel_file()
{
	if (const char * xdg = std::getenv("XDG_CONFIG_HOME"))
		return std::filesystem::path(xdg) / "wivrn" / "wheel-mask.txt";
	return std::filesystem::path(std::getenv("HOME")) / ".config" / "wivrn" / "wheel-mask.txt";
}

// Line 1: head-space rectangle. Line 2, only when anchored: x,y,z, qx,qy,qz,qw, width, height.
// WIVRN_WHEEL_MASK sets the head-space rectangle and takes priority over line 1 of the file.
wivrn::wheel_placement load_wheel_placement()
{
	wivrn::wheel_placement wheel;
	std::string rect_line;
	std::string anchor_line;
	const char * env = std::getenv("WIVRN_WHEEL_MASK");
	if (env != nullptr)
	{
		rect_line = env;
	}
	else if (std::ifstream file(wheel_file()); file)
	{
		std::getline(file, rect_line);
		std::getline(file, anchor_line);
	}

	float r[4];
	if (std::sscanf(rect_line.c_str(), "%f,%f,%f,%f", &r[0], &r[1], &r[2], &r[3]) == 4)
	{
		std::copy(r, r + 4, wheel.rect);
		wheel.enabled = true;
	}
	else if (env != nullptr)
	{
		U_LOG_W("WIVRN_WHEEL_MASK must be min_x,min_y,max_x,max_y, ignoring \"%s\"", env);
	}

	XrVector3f p{};
	XrQuaternionf q{0, 0, 0, 1};
	float size[2] = {};
	if (std::sscanf(anchor_line.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f", &p.x, &p.y, &p.z, &q.x, &q.y, &q.z, &q.w, &size[0], &size[1]) == 9)
	{
		wheel.anchored = true;
		wheel.enabled = true;
		wheel.pose.position = p;
		wheel.pose.orientation = q;
		// The wheel is a circle: a box saved by an older build becomes a circle of its average size
		wheel.width = (size[0] + size[1]) / 2;
		wheel.height = wheel.width;
	}
	return wheel;
}

void save_wheel_placement(const wivrn::wheel_placement & wheel)
{
	auto path = wheel_file();
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path);
	file << std::format("{},{},{},{}\n", wheel.rect[0], wheel.rect[1], wheel.rect[2], wheel.rect[3]);
	if (wheel.anchored)
	{
		const auto & p = wheel.pose.position;
		const auto & q = wheel.pose.orientation;
		file << std::format("{},{},{},{},{},{},{},{},{}\n", p.x, p.y, p.z, q.x, q.y, q.z, q.w, wheel.width, wheel.height);
	}
	U_LOG_I("Wheel saved to %s", path.c_str());
}
} // namespace

// Left thumbstick moves the wheel. Holding the left trigger, left thumbstick up and down scales it instead.
// Right thumbstick up and down also scales it. Right B places it at the head, right A saves.
void compositor::update_wheel_mask()
{
	auto & left = session.get_left_controller();
	auto & right = session.get_right_controller();

	auto stick = [](std::optional<xrt_input> input) {
		return input ? input->value.vec2 : xrt_vec2{};
	};
	auto pressed = [](std::optional<xrt_input> input) {
		return input and input->value.boolean;
	};
	auto deadzone = [](float v) {
		return std::abs(v) < wheel_deadzone ? 0.0f : v;
	};

	// OpenXR convention: positive x is right, positive y is up.
	xrt_vec2 move = stick(left.get_input(XRT_INPUT_TOUCH_THUMBSTICK));
	xrt_vec2 grow = stick(right.get_input(XRT_INPUT_TOUCH_THUMBSTICK));
	auto trigger = left.get_input(XRT_INPUT_TOUCH_TRIGGER_VALUE);
	bool save_down = pressed(right.get_input(XRT_INPUT_TOUCH_A_CLICK));
	bool place_down = pressed(right.get_input(XRT_INPUT_TOUCH_B_CLICK));

	// With the left trigger held, the left stick scales instead of moving
	if (trigger and trigger->value.vec1.x > 0.5f)
	{
		grow.y += move.y;
		move = {};
	}

	// Act on button presses, not on holds
	// Placing starts a fresh, upright box at the default size, facing where the head faces
	if (place_down and not wheel_place_was_down)
	{
		XrQuaternionf facing = yaw_only(head_pose.orientation);
		wivrn::wheel_placement defaults;
		wheel.anchored = true;
		wheel.enabled = true;
		wheel.width = defaults.width;
		wheel.height = defaults.height;
		wheel.pose.orientation = facing;
		wheel.pose.position = add(head_pose.position, rotate(facing, wheel_place_offset));
	}
	wheel_place_was_down = place_down;

	float mx = deadzone(move.x);
	float my = deadzone(move.y);
	float g = deadzone(std::clamp(grow.y, -1.0f, 1.0f)); // stick up grows the circle
	if (mx != 0 or my != 0 or g != 0)
		wheel.enabled = true;

	if (wheel.anchored)
	{
		// Left and right follow the circle's own sideways direction, up and down stay vertical
		XrVector3f side = rotate(wheel.pose.orientation, {mx * wheel_nudge_speed, 0, 0});
		wheel.pose.position = add(wheel.pose.position, {side.x, my * wheel_nudge_speed, side.z});
		wheel.width = std::max(wheel_min_metres, wheel.width + g * wheel_size_speed);
		wheel.height = wheel.width;
	}
	else
	{
		float dx = mx * wheel_move_speed;
		float dy = -my * wheel_move_speed; // stick up moves the rectangle up the image
		float ex = g * wheel_move_speed;
		float ey = g * wheel_move_speed;
		wheel.rect[0] += dx - ex;
		wheel.rect[2] += dx + ex;
		wheel.rect[1] += dy - ey;
		wheel.rect[3] += dy + ey;

		auto clamp_range = [](float & lo, float & hi) {
			lo = std::clamp(lo, 0.0f, 1.0f - wheel_min_uv_size);
			hi = std::clamp(hi, lo + wheel_min_uv_size, 1.0f);
		};
		clamp_range(wheel.rect[0], wheel.rect[2]);
		clamp_range(wheel.rect[1], wheel.rect[3]);
	}

	if (save_down and not wheel_save_was_down)
		save_wheel_placement(wheel);
	wheel_save_was_down = save_down;
}

// Runs once the frame's per-eye poses and FOVs are known, before foveation reads the mask.
void compositor::apply_wheel_mask(const std::array<XrPosef, 2> & poses, const std::array<XrFovf, 2> & fovs, bool flip_y)
{
	head_pose.position = {
	        (poses[0].position.x + poses[1].position.x) / 2,
	        (poses[0].position.y + poses[1].position.y) / 2,
	        (poses[0].position.z + poses[1].position.z) / 2,
	};
	head_pose.orientation = poses[0].orientation;

	wivrn::wheel_mask mask{};
	mask.enabled = wheel.enabled ? 1 : 0;
	for (int eye = 0; eye < 2; ++eye)
	{
		float * out = mask.eye + 4 * eye;
		if (not wheel.anchored)
		{
			std::copy(wheel.rect, wheel.rect + 4, out);
			continue;
		}

		// Project the wheel's circle into this eye, and use its bounding rectangle: the shader fills
		// the ellipse inside it. The part of the circle behind the eye is clipped off.
		constexpr int circle_points = 32;
		float radius = wheel.width / 2;
		std::vector<XrVector3f> circle;
		for (int i = 0; i < circle_points; ++i)
		{
			float a = 2 * std::numbers::pi_v<float> * i / circle_points;
			XrVector3f point{radius * std::cos(a), radius * std::sin(a), 0};
			circle.push_back(to_view(poses[eye], add(wheel.pose.position, rotate(wheel.pose.orientation, point))));
		}

		XrVector2f lo{2, 2};
		XrVector2f hi{-1, -1};
		for (XrVector3f point: clip_near(circle))
		{
			XrVector2f uv = project(fovs[eye], flip_y, point);
			lo.x = std::min(lo.x, uv.x);
			lo.y = std::min(lo.y, uv.y);
			hi.x = std::max(hi.x, uv.x);
			hi.y = std::max(hi.y, uv.y);
		}

		// Not clamped to the image: clamping would squash the ellipse when the circle is partly out of view
		if (lo.x <= hi.x)
		{
			out[0] = lo.x;
			out[1] = lo.y;
			out[2] = hi.x;
			out[3] = hi.y;
		}
		else
		{
			std::fill(out, out + 4, 0.0f);
		}
	}

	foveation.set_wheel_mask(mask);
}

compositor::compositor(wivrn_session & session) :
        comp_base{
                .base = {
                        .base = {
                                .info{},
                                .begin_session = method_pointer<&compositor::begin_session>,
                                .end_session = method_pointer<&compositor::end_session>,
                                .predict_frame = method_pointer<&compositor::predict_frame>,
                                .mark_frame = method_pointer<&compositor::mark_frame>,
                                .begin_frame = method_pointer<&compositor::begin_frame>,
                                .discard_frame = method_pointer<&compositor::discard_frame>,
                                .layer_commit = method_pointer<&compositor::layer_commit>,
                                .get_display_refresh_rate = method_pointer<&compositor::get_display_refresh_rate>,
                                .request_display_refresh_rate = method_pointer<&compositor::request_display_refresh_rate>,
                                .destroy = method_pointer<&compositor::destroy>,
                        },
                },
        },
        log_level(debug_get_log_option_log()),
        session(session),
        cmd_pool(vk.device, vk::CommandPoolCreateInfo{
                                    .flags = vk::CommandPoolCreateFlagBits::eTransient,
                                    .queueFamilyIndex = vk.queue.family_index,
                            }),
        query_pool(vk.device, vk::QueryPoolCreateInfo{
                                      .queryType = vk::QueryType::eTimestamp,
                                      .queryCount = 3,
                              }),
        settings(get_encoder_settings(vk, session)),
        images{make_images(vk, cmd_pool, settings)},
        cmd{std::move(vk.device.allocateCommandBuffers({.commandPool = *cmd_pool, .commandBufferCount = 1})[0])},
        sem{make_semaphore(vk)},
        frame_rate(settings[0].fps),
        pacer(U_TIME_1S_IN_NS / frame_rate),
        squasher(vk, render_extent(session.get_info())),
        foveation(vk, images[0].image.info().extent)
{
	comp_base * c_base = this;
	wheel = load_wheel_placement();
	// Ensure we can safely cast pointers
	assert(intptr_t(&base) == intptr_t(this));
	auto res = vk_init_from_given(
	        &c_base->vk,
	        (PFN_vkGetInstanceProcAddr)vk.instance.getProcAddr("vkGetInstanceProcAddr"),
	        *vk.instance,
	        *vk.physical_device,
	        *vk.device,
	        vk.queue.family_index,
	        0,
	        vk.has_device_ext(VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME),
	        vk.has_device_ext(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME),
	        std::get<vk::PhysicalDeviceVulkan12Features>(vk.feat).timelineSemaphore,
	        true, // in Vulkan 1.2
	        vk.has_instance_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME),
	        log_level);
	vk::detail::resultCheck(vk::Result(res), "vk_init_from_given");

	c_base->vk.version = vk_bundle::api_version;
	// vk_init_from_given can't enable calibrated timestamps; do it here.
#ifdef VK_EXT_calibrated_timestamps
	c_base->vk.has_EXT_calibrated_timestamps = vk.has_device_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
#endif

	// Share monado's vk_bundle so gpu_timestamp_pool reuses its calibration cache.
	wivrn::trace::set_calibration_source(&c_base->vk);

	// vk_init_from_given assumes a graphics queue was provided
	c_base->vk.graphics_queue = nullptr;

	// Monado submits to the main queue from IPC client threads under
	// main_queue->mutex: our own submissions must take the same lock.
	vk::detail::resultCheck(vk::Result(vk_init_mutex(&c_base->vk)), "vk_init_mutex");
	if (c_base->vk.main_queue)
		vk.queue.mutex.share(c_base->vk.main_queue->mutex.mutex);

	{
		comp_vulkan_formats formats{};
		comp_vulkan_formats_check(&c_base->vk, &formats);
		comp_vulkan_formats_copy_to_info(&formats, &base.base.info);
		comp_vulkan_formats_log(log_level, &formats);
	}

	comp_base_init(this);

	// Tie the lifetimes of swapchains to Vulkan.
	xrt_result_t xret = comp_swapchain_shared_init(&cscs, &c_base->vk);
	if (xret != XRT_SUCCESS)
		throw std::runtime_error("comp_swapchain_shared_init failed");

	print_encoders(settings);
	for (auto [i, settings]: std::ranges::enumerate_view(settings))
		encoders[i] = video_encoder::create(vk, settings, i);
	send_video_stream_description();

	u_var_add_root(this, "Compositor", false);
	u_var_add_f32_timing(this, &squasher_times.var, "layers processing");
	u_var_add_f32_timing(this, &foveation_times.var, "foveation");

	// Start the thread after everything is initialized
	encoder_thread = std::jthread{[&](std::stop_token t) { encoder_work(t); }};
}

compositor::~compositor()
{
	u_var_remove_root(this);
	encoder_thread.request_stop();
	encode_request = -2;
	encode_request.notify_all();
	comp_base * c_base = this;
	comp_swapchain_shared_garbage_collect(&cscs);
	comp_swapchain_shared_destroy(&cscs, &c_base->vk);
	comp_base_fini(this);
}

xrt_system_compositor_info compositor::sys_info() const
{
	const auto & info = session.get_info();
	auto [prop, dev_id] = vk.physical_device.getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceIDProperties>();
	xrt_system_compositor_info res{
	        .view_type_count = 1,
	        .view_types = {
	                XRT_VIEW_TYPE_STEREO,
	        },
	        .max_layers = squasher.max_layers(prop.properties),
	        .supported_blend_modes = {
	                XRT_BLEND_MODE_OPAQUE,
	                XRT_BLEND_MODE_ALPHA_BLEND,
	        },
	        .supported_blend_mode_count = uint8_t(1 + info.passthrough),
	        .refresh_rate_count = std::min<uint32_t>(info.available_refresh_rates.size(), std::size(res.refresh_rates_hz)),
	        .supports_fov_mutable = true,
	};

	std::ranges::copy(dev_id.deviceUUID, res.compositor_vk_deviceUUID.data);
	std::ranges::copy(dev_id.deviceUUID, res.client_vk_deviceUUID.data);
	std::ranges::copy(std::span(info.available_refresh_rates).subspan(0, res.refresh_rate_count), res.refresh_rates_hz);
	return res;
}

void compositor::set_bitrate(uint32_t bitrate)
{
	for (auto & encoder: encoders)
		encoder->set_bitrate(bitrate);
}

void compositor::set_framerate(float hz)
{
	if (frame_rate.exchange(hz) == hz)
		return;
	U_LOG_IFL_D(log_level, "Framerate change from %.0f to %.0f", frame_rate.load(), hz);
	pacer.set_frame_duration(U_TIME_1S_IN_NS / hz);
	for (auto & encoder: encoders)
		encoder->set_framerate(hz);
}

void compositor::update_foveation(const from_headset::foveation & packet)
{
	foveation.update_foveation(packet);
}

void compositor::resume()
{
	for (auto & encoder: encoders)
		encoder->reset();
	send_video_stream_description();
}

void compositor::on_feedback(const from_headset::feedback & feedback, const clock_offset & o)
{
	uint8_t stream = feedback.stream_index;
	if (stream >= encoders.size())
		return;
	encoders[stream]->on_feedback(feedback);
	if (not o)
		return;
	pacer.on_feedback(feedback, o);
}

} // namespace wivrn
