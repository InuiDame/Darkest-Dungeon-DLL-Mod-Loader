// DD.ModLoader native 插件的 header-only C++ 封装，隐藏插件句柄，提供 std::format 日志、
// lambda 形式的回调。只用 STL 自身，不跨 ABI 边界传递 STL 对象。
//
// 用法：
//   static dd::Runtime g_dd;
//   DD_EXPORT bool DD_CALL DDPlugin_Query(DDPluginInfo* out) { ...; return true; }
//   DD_EXPORT bool DD_CALL DDPlugin_Load(const DDApi* api) { return g_dd.init(api) && setup(); }
#pragma once

#include "dd_api.h"

#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dd {

class Log
{
public:
	void init(const DDLogService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	void write(int level, std::string_view message) const
	{
		if (service_) {
			const std::string text(message);
			service_->write(plugin_, level, text.c_str());
		}
	}

	template <class... Args>
	void info(std::format_string<Args...> fmt, Args&&... args) const
	{
		write(DD_LOG_INFO, std::format(fmt, std::forward<Args>(args)...));
	}

	template <class... Args>
	void warn(std::format_string<Args...> fmt, Args&&... args) const
	{
		write(DD_LOG_WARN, std::format(fmt, std::forward<Args>(args)...));
	}

	template <class... Args>
	void error(std::format_string<Args...> fmt, Args&&... args) const
	{
		write(DD_LOG_ERROR, std::format(fmt, std::forward<Args>(args)...));
	}

private:
	const DDLogService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

class Tasks
{
public:
	void init(const DDTaskService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	// std::function 由 Tasks 持有，任务执行后释放。需要取消的场景直接用 service()。
	DDTaskId post(std::function<void()> fn, std::uint32_t delayMs = 0) const
	{
		if (!service_) {
			return 0;
		}
		auto* holder = new std::function<void()>(std::move(fn));
		const DDTaskId id = delayMs == 0 ? service_->post(plugin_, &Thunk, holder)
		                                 : service_->post_delayed(plugin_, &Thunk, holder, delayMs);
		if (id == 0) {
			delete holder;
		}
		return id;
	}

	bool is_main_thread() const { return service_ && service_->is_main_thread(); }
	const DDTaskService* service() const { return service_; }

private:
	static void DD_CALL Thunk(void* user)
	{
		auto* fn = static_cast<std::function<void()>*>(user);
		(*fn)();
		delete fn;
	}

	const DDTaskService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

class Lifecycle
{
public:
	using Callback = std::function<void(const DDPhaseEvent&)>;

	void init(const DDLifecycleService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	// 回调对象常驻到进程结束（P1 没有热卸载）
	DDSubscriptionId subscribe(DDPhase phase, Callback fn) const
	{
		if (!service_) {
			return 0;
		}
		auto* holder = new Callback(std::move(fn));
		const auto id = service_->subscribe(plugin_, phase, &Thunk, holder);
		if (id == 0) {
			delete holder;
		}
		return id;
	}

	bool campaign_active() const { return service_ && service_->campaign_active(); }
	const DDLifecycleService* service() const { return service_; }

private:
	static void DD_CALL Thunk(const DDPhaseEvent* event, void* user)
	{
		(*static_cast<Callback*>(user))(*event);
	}

	const DDLifecycleService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

class Hooks
{
public:
	void init(const DDHookService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	bool scan(const char* pattern, void** out) const { return service_ && service_->scan(pattern, out); }

	const char* text(DDHookResult result) const { return service_ ? service_->result_text(result) : "no service"; }

	// 内联 hook。expected 必须是入口的真实字节。
	template <class Fn>
	DDHookResult install_inline(const char* name, void* target, std::initializer_list<std::uint8_t> expected,
		Fn detour, Fn* original, DDHookHandle* handle = nullptr, std::int32_t priority = 0) const
	{
		DDHookSpec spec{};
		spec.size = sizeof(spec);
		spec.kind = DD_HOOK_INLINE;
		spec.name = name;
		spec.target = target;
		const std::vector<std::uint8_t> bytes(expected);
		spec.expected = bytes.data();
		spec.expected_len = static_cast<std::uint32_t>(bytes.size());
		spec.priority = priority;
		spec.detour = reinterpret_cast<void*>(detour);
		spec.original = reinterpret_cast<void**>(original);
		return install_spec(spec, handle);
	}

	// 虚表 hook。slot 是虚表槽位地址（void**）。expectedValue 非空时要求槽位当前等于它。
	template <class Fn>
	DDHookResult install_vtable(const char* name, void** slot, void* expectedValue, Fn detour, Fn* original,
		DDHookHandle* handle = nullptr, std::int32_t priority = 0) const
	{
		DDHookSpec spec{};
		spec.size = sizeof(spec);
		spec.kind = DD_HOOK_VTABLE;
		spec.name = name;
		spec.target = slot;
		spec.expected_value = expectedValue;
		spec.priority = priority;
		spec.detour = reinterpret_cast<void*>(detour);
		spec.original = reinterpret_cast<void**>(original);
		return install_spec(spec, handle);
	}

	DDHookResult remove(DDHookHandle handle) const
	{
		return service_ ? service_->remove(plugin_, handle) : DD_HOOK_E_BAD_ARGUMENT;
	}

	const DDHookService* service() const { return service_; }

private:
	DDHookResult install_spec(const DDHookSpec& spec, DDHookHandle* handle) const
	{
		if (!service_) {
			return DD_HOOK_E_BAD_ARGUMENT;
		}
		DDHookHandle local = 0;
		const auto result = service_->install(plugin_, &spec, handle ? handle : &local);
		return result;
	}

	const DDHookService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

class GameInfo
{
public:
	void init(const DDGameInfoService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	const DDGameInfoService* operator->() const { return service_; }
	const char* plugin_path() const { return service_ ? service_->plugin_path(plugin_) : ""; }
	explicit operator bool() const { return service_ != nullptr; }

private:
	const DDGameInfoService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

// 事件订阅和负载访问的封装。回调收到的 DDEvent* 只在回调期间有效。
class Events
{
public:
	using Callback = std::function<void(DDEvent*)>;

	void init(const DDEventsService* service, DDPluginHandle plugin)
	{
		service_ = service;
		plugin_ = plugin;
	}

	// 订阅失败返回 0，原因在日志里。回调对象常驻到进程结束。
	DDEventSubId on(const char* topic, Callback fn, std::int32_t priority = 0, DDEventScope scope = DD_EVENT_SCOPE_PLUGIN) const
	{
		if (!service_) {
			return 0;
		}
		DDEventOptions options{ sizeof(DDEventOptions), priority, static_cast<std::uint32_t>(scope) };
		auto* holder = new Callback(std::move(fn));
		const auto id = service_->subscribe(plugin_, topic, &Thunk, holder, &options);
		if (id == 0) {
			delete holder;
		}
		return id;
	}

	bool off(DDEventSubId id) const { return service_ && service_->unsubscribe(plugin_, id); }

	std::int64_t get_int(const DDEvent* event, const char* field, std::int64_t fallback = 0) const
	{
		std::int64_t value = fallback;
		return service_ && service_->get_int(event, field, &value) == DD_EV_OK ? value : fallback;
	}

	double get_float(const DDEvent* event, const char* field, double fallback = 0.0) const
	{
		double value = fallback;
		return service_ && service_->get_float(event, field, &value) == DD_EV_OK ? value : fallback;
	}

	bool get_bool(const DDEvent* event, const char* field, bool fallback = false) const
	{
		bool value = fallback;
		return service_ && service_->get_bool(event, field, &value) == DD_EV_OK ? value : fallback;
	}

	const char* get_string(const DDEvent* event, const char* field, const char* fallback = "") const
	{
		const char* value = fallback;
		return service_ && service_->get_string(event, field, &value) == DD_EV_OK ? value : fallback;
	}

	void* get_pointer(const DDEvent* event, const char* field) const
	{
		void* value = nullptr;
		return service_ && service_->get_pointer(event, field, &value) == DD_EV_OK ? value : nullptr;
	}

	bool set_int(DDEvent* event, const char* field, std::int64_t value) const
	{
		return service_ && service_->set_int(event, field, value) == DD_EV_OK;
	}

	bool set_float(DDEvent* event, const char* field, double value) const
	{
		return service_ && service_->set_float(event, field, value) == DD_EV_OK;
	}

	bool set_bool(DDEvent* event, const char* field, bool value) const
	{
		return service_ && service_->set_bool(event, field, value) == DD_EV_OK;
	}

	bool cancel(DDEvent* event) const { return service_ && service_->cancel(event) == DD_EV_OK; }
	std::uint32_t flags(const DDEvent* event) const { return service_ ? service_->event_flags(event) : 0; }
	const char* topic(const DDEvent* event) const { return service_ ? service_->event_topic(event) : ""; }
	std::uint32_t origin_thread(const DDEvent* event) const { return service_ ? service_->event_origin_thread(event) : 0; }

	const DDEventsService* service() const { return service_; }
	explicit operator bool() const { return service_ != nullptr; }

private:
	static void DD_CALL Thunk(DDEvent* event, void* user) { (*static_cast<Callback*>(user))(event); }

	const DDEventsService* service_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
};

// 把一次 Load 拿到的全部服务收在一处
class Runtime
{
public:
	bool init(const DDApi* api)
	{
		if (!api || api->size < sizeof(DDApi) || !api->query) {
			return false;
		}
		api_ = api;
		plugin_ = api->plugin;
		log_.init(static_cast<const DDLogService*>(api->query(DD_SERVICE_LOG, DD_SERVICE_LOG_VERSION)), plugin_);
		tasks_.init(static_cast<const DDTaskService*>(api->query(DD_SERVICE_TASK, DD_SERVICE_TASK_VERSION)), plugin_);
		lifecycle_.init(
			static_cast<const DDLifecycleService*>(api->query(DD_SERVICE_LIFECYCLE, DD_SERVICE_LIFECYCLE_VERSION)),
			plugin_);
		hooks_.init(static_cast<const DDHookService*>(api->query(DD_SERVICE_HOOK, DD_SERVICE_HOOK_VERSION)), plugin_);
		gameInfo_.init(
			static_cast<const DDGameInfoService*>(api->query(DD_SERVICE_GAME_INFO, DD_SERVICE_GAME_INFO_VERSION)),
			plugin_);
		// events 服务是 P2 才有的，老 DDCore 上为空，Events 对象变成无操作
		events_.init(static_cast<const DDEventsService*>(api->query(DD_SERVICE_EVENTS, DD_SERVICE_EVENTS_VERSION)),
			plugin_);
		return true;
	}

	const DDApi* api() const { return api_; }
	DDPluginHandle plugin() const { return plugin_; }
	const Log& log() const { return log_; }
	const Tasks& tasks() const { return tasks_; }
	const Lifecycle& lifecycle() const { return lifecycle_; }
	const Hooks& hooks() const { return hooks_; }
	const GameInfo& game() const { return gameInfo_; }
	const Events& events() const { return events_; }

private:
	const DDApi* api_ = nullptr;
	DDPluginHandle plugin_ = nullptr;
	Log log_;
	Tasks tasks_;
	Lifecycle lifecycle_;
	Hooks hooks_;
	GameInfo gameInfo_;
	Events events_;
};

}  // namespace dd
