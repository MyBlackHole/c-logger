#define _GNU_SOURCE
#include "logger_internal.h"
#include "support.h"

/* 仅验证真实私有头的内联封装及独立原子对象，不链接生产库，
 * 不将单线程值/写回检查当作并发内存序或完整状态机证明。
 */
static unsigned object_calls;
static unsigned value_calls;
static unsigned expected_calls;

static _Atomic logger_state_t *state_argument(_Atomic logger_state_t *object)
{
	++object_calls;
	return object;
}

static _Atomic logger_level_t *level_argument(_Atomic logger_level_t *object)
{
	++object_calls;
	return object;
}

static logger_state_t state_value(logger_state_t value)
{
	++value_calls;
	return value;
}

static logger_level_t level_value(logger_level_t value)
{
	++value_calls;
	return value;
}

static logger_state_t *expected_argument(logger_state_t *expected)
{
	++expected_calls;
	return expected;
}

static void check_evaluations(unsigned has_expected)
{
	CHECK(object_calls == 1);
	CHECK(value_calls == 1);
	CHECK(expected_calls == has_expected);
	object_calls = 0;
	value_calls = 0;
	expected_calls = 0;
}

int main(void)
{
	_Atomic logger_state_t state;
	_Atomic logger_level_t level;
	logger_state_t expected = LOGGER_STATE_RUNNING;
	int result;

	logger_state_init(state_argument(&state),
			  state_value(LOGGER_STATE_CREATED));
	check_evaluations(0);
	CHECK(atomic_load_explicit(&state, memory_order_relaxed) ==
	      LOGGER_STATE_CREATED);

	/* 故意比较失败：对象不变，调用方的 expected 得到实际状态。
	 * 下一次直接使用这个写回值，拒绝在封装内复制后丢失写回。
	 */
	result = logger_state_try_publish(state_argument(&state),
					  expected_argument(&expected),
					  state_value(LOGGER_STATE_STOPPING));
	check_evaluations(1);
	CHECK(result == 0);
	CHECK(expected == LOGGER_STATE_CREATED);
	CHECK(atomic_load_explicit(&state, memory_order_relaxed) ==
	      LOGGER_STATE_CREATED);

	result = logger_state_try_publish(state_argument(&state),
					  expected_argument(&expected),
					  state_value(LOGGER_STATE_RUNNING));
	check_evaluations(1);
	CHECK(result == 1);
	CHECK(expected == LOGGER_STATE_CREATED);
	CHECK(atomic_load_explicit(&state, memory_order_relaxed) ==
	      LOGGER_STATE_RUNNING);

	/* 两种枚举的端值各由真实 init/store 写入，不只核对返回值。 */
	logger_state_store_release(state_argument(&state),
				   state_value(LOGGER_STATE_STOPPED));
	check_evaluations(0);
	CHECK(atomic_load_explicit(&state, memory_order_relaxed) ==
	      LOGGER_STATE_STOPPED);

	logger_level_init(level_argument(&level), level_value(LOGGER_TRACE));
	check_evaluations(0);
	CHECK(atomic_load_explicit(&level, memory_order_relaxed) ==
	      LOGGER_TRACE);
	logger_level_store_relaxed(level_argument(&level),
				   level_value(LOGGER_OFF));
	check_evaluations(0);
	CHECK(atomic_load_explicit(&level, memory_order_relaxed) == LOGGER_OFF);

	CHECK(puts("ENUM_ATOMIC_PASS=writeback") >= 0);
	CHECK(fflush(stdout) == 0);
	return 0;
}
