/* Copyright (c) 2019-2025, Arm Limited and Contributors
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 the "License";
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "platform.h"

#include <algorithm>
#include <ctime>
#include <iostream>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <spdlog/async_logger.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "core/util/logging.hpp"
#include "force_close/force_close.h"
#include "platform/plugins/plugin.h"
#include "vulkan_sample.h"

namespace vkb
{
const uint32_t Platform::MIN_WINDOW_WIDTH  = 420;
const uint32_t Platform::MIN_WINDOW_HEIGHT = 320;

Platform::Platform(const PlatformContext &context)
{
	arguments = context.arguments();
}

ExitCode Platform::initialize(const std::vector<Plugin *> &plugins_)
{
	plugins = plugins_;

	auto sinks = get_platform_sinks();

	auto logger = std::make_shared<spdlog::logger>("logger", sinks.begin(), sinks.end());

#ifdef VKB_DEBUG
	logger->set_level(spdlog::level::debug);
#else
	logger->set_level(spdlog::level::info);
#endif

	logger->set_pattern(LOGGER_FORMAT);
	spdlog::set_default_logger(logger);

	LOGI("Logger initialized");

	// To get the error messages formatted as we like them to have, exit after initializing the logger, earliest
	if (arguments.empty())
	{
		return ExitCode::NoSample;
	}
	else if (std::ranges::any_of(arguments, [](auto const &arg) { return arg == "-h" || arg == "--help"; }))
	{
		return ExitCode::Help;
	}

	for (auto const &plugin : plugins)
	{
		plugin->set_platform(this);
		for (auto const &command : plugin->get_commands())
		{
			auto [it, inserted] = command_map.insert(std::make_pair(command.first, plugin));
			if (!inserted)
			{
				LOGE("Command \"{}\" from plugin \"{}\" is already listed for plugin \"{}\"!", command.first, plugin->get_name(), it->second->get_name());
			}
		}
		for (auto const &option : plugin->get_options())
		{
			auto [it, inserted] = option_map.insert(std::make_pair(option.first, plugin));
			if (!inserted)
			{
				LOGE("Option \"{}\" from plugin \"{}\" is already listed for plugin \"{}\"!", option.first, plugin->get_name(), it->second->get_name());
			}
		}
	}

	std::deque<std::string> argumentDeque(arguments.begin(), arguments.end());

	// the arguments have to start with a command
	auto commandIt = command_map.find(argumentDeque[0]);
	if (commandIt == command_map.end())
	{
		LOGE("Command \"{}\" is unknown!", argumentDeque[0]);
		return ExitCode::Help;
	}
	else if (commandIt->second->handle_command(argumentDeque))
	{
		register_hooks(commandIt->second);
	}
	else
	{
		LOGE("Command \"{}\" advertised by plugin \"{}\" was not handled!", argumentDeque[0], commandIt->second->get_name());
		return ExitCode::Help;
	}
	// and then there are options only
	while (!argumentDeque.empty())
	{
		if (argumentDeque[0].substr(0, 2) != "--")
		{
			LOGE("Option \"{}\" does not start with \"--\"!", argumentDeque[0]);
			return ExitCode::Help;
		}
		auto optionIt = option_map.find(argumentDeque[0].substr(2));
		if (commandIt == command_map.end())
		{
			LOGE("Option \"{}\" is unknown!", argumentDeque[0]);
			return ExitCode::Help;
		}
		else if (optionIt->second->handle_option(argumentDeque))
		{
			register_hooks(optionIt->second);
		}
		else
		{
			LOGE("Option \"{}\" advertised by plugin \"{}\" was not handled!", argumentDeque[0], optionIt->second->get_name());
			return ExitCode::Help;
		}
	}
	// Now that all options are handled, trigger the command
	commandIt->second->trigger_command();

	// Platform has been closed by a plugins initialization phase
	if (close_requested)
	{
		return ExitCode::Close;
	}

	if (!app_requested())
	{
		return ExitCode::NoSample;
	}

	create_window(window_properties);

	if (!window)
	{
		LOGE("Window creation failed!");
		return ExitCode::FatalError;
	}

	return ExitCode::Success;
}

void Platform::register_hooks(Plugin *plugin)
{
	auto &plugin_hooks = plugin->get_hooks();
	for (auto hook : plugin_hooks)
	{
		auto it = hooks.find(hook);

		if (it == hooks.end())
		{
			auto r = hooks.emplace(hook, std::vector<Plugin *>{});
			assert(r.second);
			it = r.first;
		}

		if (std::ranges::none_of(it->second, [plugin](auto p) { return p == plugin; }))
		{
			it->second.emplace_back(plugin);
		}
	}

	if (std::ranges::none_of(active_plugins, [plugin](auto p) { return p == plugin; }))
	{
		active_plugins.emplace_back(plugin);
	}
}

ExitCode Platform::main_loop_frame()
{
	try
	{
		// Load the requested app
		if (app_requested())
		{
			if (!start_app())
			{
				throw std::runtime_error("Failed to load requested application");
			}

			// Compensate for load times of the app by rendering the first frame pre-emptively
			timer.tick<Timer::Seconds>();
			active_app->update(0.01667f);
		}

		if (!active_app)
		{
			return ExitCode::NoSample;
		}

		update();

		if (active_app->should_close())
		{
			std::string id = active_app->get_name();
			on_app_close(id);
			active_app->finish();
		}

		window->process_events();

		if (window->should_close() || close_requested)
		{
			return ExitCode::Close;
		}
	}
	catch (std::exception &e)
	{
		LOGE("Error Message: {}", e.what());
		LOGE("Failed when running application {}", active_app->get_name());

		on_app_error(active_app->get_name());

		if (app_requested())
		{
			LOGI("Attempting to load next application");
		}
		else
		{
			set_last_error(e.what());
			return ExitCode::FatalError;
		}
	}

	return ExitCode::Success;
}

ExitCode Platform::main_loop()
{
	ExitCode exit_code = ExitCode::Success;
	while (exit_code == ExitCode::Success)
	{
		exit_code = main_loop_frame();
	}

	return exit_code;
}

void Platform::update()
{
	auto delta_time = static_cast<float>(timer.tick<Timer::Seconds>());

	if (focused || always_render)
	{
		on_update(delta_time);

		if (fixed_simulation_fps)
		{
			delta_time = simulation_frame_time;
		}

		active_app->update_overlay(delta_time, [=, this]() {
			on_update_ui_overlay(*active_app->get_drawer());
		});
		active_app->update(delta_time);

		if (auto *app = dynamic_cast<VulkanSampleCpp *>(active_app.get()))
		{
			if (app->has_render_context())
			{
				on_post_draw(reinterpret_cast<vkb::RenderContext &>(app->get_render_context()));
			}
		}
		else if (auto *app = dynamic_cast<VulkanSampleC *>(active_app.get()))
		{
			if (app->has_render_context())
			{
				on_post_draw(app->get_render_context());
			}
		}
	}
}

void Platform::terminate(ExitCode code)
{
	if (code == ExitCode::Help)
	{
		LOGI("");
		LOGI("\tVulkan Samples");
		LOGI("");
		LOGI("\t\tA collection of samples to demonstrate the Vulkan best practice.");
		LOGI("");
		LOGI("\tUsage: vulkan_samples [OPTIONS]");
		LOGI("");
		LOGI("\t\tOptions:");
		LOGI("\t\t\t-h,--help                   Print this help message and exit");

		// determine the width for the commands/options
		size_t width = 4;        // minimal width for "help"
		for (auto plugin : plugins)
		{
			for (auto const &command : plugin->get_commands())
			{
				width = std::max(width, command.first.length());
			}
			for (auto const &option : plugin->get_options())
			{
				width = std::max(width, option.first.length());
			}
		}

		for (auto plugin : plugins)
		{
			plugin->log_help(width + 2);
		}
	}

	if (code == ExitCode::NoSample)
	{
		LOGI("");
		LOGI("No sample was requested or the selected sample does not exist");
		LOGI("");
		LOGI("To run a specific sample use the \"sample\" argument, e.g.");
		LOGI("");
		LOGI("\tvulkan_samples sample hello_triangle");
		LOGI("");
		LOGI("To get a list of available samples, use the \"samples\" argument")
		LOGI("To get a list of available command line options, use the \"-h\" or \"--help\" argument");
		LOGI("");
	}

	if (active_app)
	{
		std::string id = active_app->get_name();
		on_app_close(id);
		active_app->finish();
	}

	active_app.reset();
	window.reset();

	spdlog::drop_all();

	on_platform_close();

#ifdef PLATFORM__WINDOWS
	// Halt on all unsuccessful exit codes unless ForceClose is in use
	if (code != ExitCode::Success && !using_plugin<::plugins::ForceClose>())
	{
		std::cout << "Press return to continue";
		std::cin.get();
	}
#endif
}

void Platform::close()
{
	if (window)
	{
		window->close();
	}

	// Fallback incase a window is not yet in use
	close_requested = true;
}

void Platform::force_simulation_fps(float fps)
{
	fixed_simulation_fps  = true;
	simulation_frame_time = 1 / fps;
}

void Platform::force_render(bool should_always_render)
{
	always_render = should_always_render;
}

void Platform::disable_input_processing()
{
	process_input_events = false;
}

void Platform::set_focus(bool _focused)
{
	focused = _focused;
}

void Platform::set_window_properties(const Window::OptionalProperties &properties)
{
	window_properties.title         = properties.title.has_value() ? properties.title.value() : window_properties.title;
	window_properties.mode          = properties.mode.has_value() ? properties.mode.value() : window_properties.mode;
	window_properties.resizable     = properties.resizable.has_value() ? properties.resizable.value() : window_properties.resizable;
	window_properties.vsync         = properties.vsync.has_value() ? properties.vsync.value() : window_properties.vsync;
	window_properties.extent.width  = properties.extent.width.has_value() ? properties.extent.width.value() : window_properties.extent.width;
	window_properties.extent.height = properties.extent.height.has_value() ? properties.extent.height.value() : window_properties.extent.height;
}

std::string &Platform::get_last_error()
{
	return last_error;
}

Application &Platform::get_app()
{
	assert(active_app && "Application is not valid");
	return *active_app;
}

Application &Platform::get_app() const
{
	assert(active_app && "Application is not valid");
	return *active_app;
}

Window &Platform::get_window()
{
	return *window;
}

void Platform::set_last_error(const std::string &error)
{
	last_error = error;
}

std::vector<spdlog::sink_ptr> Platform::get_platform_sinks()
{
	std::vector<spdlog::sink_ptr> sinks;
	sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
	return sinks;
}

bool Platform::app_requested()
{
	return requested_app != nullptr;
}

void Platform::request_application(const apps::AppInfo *app)
{
	requested_app = app;
}

bool Platform::start_app()
{
	auto *requested_app_info = requested_app;
	// Reset early incase error in preparation stage
	requested_app = nullptr;

	if (active_app)
	{
		auto execution_time = timer.stop();
		LOGI("Closing App (Runtime: {:.1f})", execution_time);

		auto app_id = active_app->get_name();

		active_app->finish();
	}

	active_app = requested_app_info->create();

	if (!active_app)
	{
		LOGE("Failed to create a valid vulkan app.");
		return false;
	}
	auto sample_info = static_cast<const apps::SampleInfo *>(requested_app_info);
	active_app->set_name(sample_info->name);

	if (!active_app->prepare({false, window.get()}))
	{
		LOGE("Failed to prepare vulkan app.");
		return false;
	}

	on_app_start(requested_app_info->id);

	return true;
}

void Platform::input_event(const InputEvent &input_event)
{
	if (process_input_events && active_app)
	{
		active_app->input_event(input_event);
	}

	if (input_event.get_source() == EventSource::Keyboard)
	{
		const auto &key_event = static_cast<const KeyInputEvent &>(input_event);

		if (key_event.get_code() == KeyCode::Back ||
		    key_event.get_code() == KeyCode::Escape)
		{
			close();
		}
	}
}

void Platform::resize(uint32_t width, uint32_t height)
{
	auto extent = Window::Extent{std::max<uint32_t>(width, MIN_WINDOW_WIDTH), std::max<uint32_t>(height, MIN_WINDOW_HEIGHT)};
	if ((window) && (width > 0) && (height > 0))
	{
		auto actual_extent = window->resize(extent);

		if (active_app)
		{
			active_app->resize(actual_extent.width, actual_extent.height);
		}
	}
}

#define HOOK(enum, func)                \
	static auto res = hooks.find(enum); \
	if (res != hooks.end())             \
	{                                   \
		for (auto plugin : res->second) \
		{                               \
			plugin->func;               \
		}                               \
	}

void Platform::on_post_draw(RenderContext &context)
{
	HOOK(Hook::PostDraw, on_post_draw(context));
}

void Platform::on_app_error(const std::string &app_id)
{
	HOOK(Hook::OnAppError, on_app_error(app_id));
}

void Platform::on_update(float delta_time)
{
	HOOK(Hook::OnUpdate, on_update(delta_time));
}

void Platform::on_app_start(const std::string &app_id)
{
	HOOK(Hook::OnAppStart, on_app_start(app_id));
}

void Platform::on_app_close(const std::string &app_id)
{
	HOOK(Hook::OnAppClose, on_app_close(app_id));
}

void Platform::on_platform_close()
{
	HOOK(Hook::OnPlatformClose, on_platform_close());
}

void Platform::on_update_ui_overlay(vkb::Drawer &drawer)
{
	HOOK(Hook::OnUpdateUi, on_update_ui_overlay(drawer));
}

#undef HOOK

}        // namespace vkb
