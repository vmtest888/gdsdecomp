#pragma once
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

// We have a SceneTree and a regular node class that call into our singleton because of the editor; normally we would be able to
// just use the SceneTree class, but when running in editor mode, Main::start() always sets the main loop to the base SceneTree class and
// that cannot be overridden.

class GDREMainLoop : public Object {
	GDCLASS(GDREMainLoop, Object);
	static GDREMainLoop *singleton;
	static bool testing;

	double last_physics_process_time = 0.0;
	double last_process_time = 0.0;
	bool processing = false;
	bool running_process_calls = false;
	Mutex next_process_calls_mutex;
	Vector<Callable> next_process_calls;

	bool _wait_until_next_frame(int64_t p_time_usec, bool called_from_process);
	void _process_next_process_calls();

protected:
	static void _bind_methods();

public:
	static constexpr int64_t FRAME_TIME_US = 10000;
	static GDREMainLoop *get_singleton();
	void initialize();
	void iteration_prepare();
	bool physics_process(double p_time);
	void iteration_end();
	bool process(double p_time);
	void finalize();

	static bool call_on_next_process(const Callable &p_callable);
	static bool iteration(bool p_no_delay = false);
	static bool wait_until_next_frame(int64_t p_time_usec = FRAME_TIME_US);

#ifdef TESTS_ENABLED
	static bool is_testing();
	static void set_is_testing(bool p_is_testing);
#endif

	GDREMainLoop();
	~GDREMainLoop();
};

class GDRESceneTree : public SceneTree {
	GDCLASS(GDRESceneTree, SceneTree);

public:
	virtual void initialize() override;
	virtual void iteration_prepare() override;
	virtual bool physics_process(double p_time) override;
	virtual void iteration_end() override;
	virtual bool process(double p_time) override;
	virtual void finalize() override;
};

#ifdef TOOLS_ENABLED
class GDREMainLoopNode : public Node {
	GDCLASS(GDREMainLoopNode, Node);

protected:
	bool _init();
	void _notification(int p_what);

public:
	static void setup();

	GDREMainLoopNode();
	~GDREMainLoopNode();
};
#endif
