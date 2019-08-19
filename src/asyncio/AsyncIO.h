#pragma once

#include <memory>
#include <functional>

#include <simplyfile/Epoll.h>

#include <future>
#include <utility>

namespace asyncio
{

namespace detail {

struct SchedulerState;

struct TesterBase {
    virtual bool test() const noexcept = 0;
};
template<typename Func>
struct Tester : TesterBase {
    Func f;
    Tester(Func _f) : f{std::move(_f)} {}
    bool test() const noexcept override {
        return f();
    }
};

void await(TesterBase const& tester);

template <typename Lambda, typename ... Args>
auto capture_call(Lambda&& lambda, Args&& ... args){
    return [
        lambda = std::forward<Lambda>(lambda),
        capture_args = std::make_tuple(std::forward<Args>(args) ...)
    ](auto&& ... original_args)mutable{
        return std::apply([&lambda](auto&& ... args){
            lambda(std::forward<decltype(args)>(args) ...);
        }, std::tuple_cat(
            std::forward_as_tuple(original_args ...),
            std::apply([](auto&& ... args){
                return std::forward_as_tuple< Args ... >(
                    std::move(args) ...);
            }, std::move(capture_args))
        ));
    };
}

}

template<typename RetType>
RetType await(std::future<RetType> future) {
    detail::await(detail::Tester{[&]() {
        auto wait_res = future.wait_until(std::chrono::system_clock::time_point{});
        return wait_res != std::future_status::timeout;
    }});
    return future.get();
}


std::future<void> readable(int fd);
std::future<void> writable(int fd);

struct Scheduler : private simplyfile::Epoll {
    using WrappedTask = std::packaged_task<void()>;
    static constexpr std::size_t defautl_stack_size = (1<<16);
	Scheduler();
    ~Scheduler();

    template<typename FuncType, typename... Args>
    std::future<std::invoke_result_t<FuncType, Args...>> run(FuncType&& func, Args &&... args) {
        using TaskT = std::packaged_task<std::invoke_result_t<FuncType, Args...>()>;
        TaskT task{detail::capture_call(std::forward<FuncType>(func), std::forward<Args>(args)...)};
        auto fut = task.get_future();
        enque(WrappedTask{[f=std::move(task)] () mutable { f(); }}, defautl_stack_size);
        return fut;
    }

	void work(int maxEvents=1, int timeout_ms=-1);

    using simplyfile::Epoll::addFD;
    using simplyfile::Epoll::rmFD;
    using simplyfile::Epoll::wakeup;
    using simplyfile::Epoll::getRuntimes;

private:
    void enque(WrappedTask task, std::size_t stack_size);
	std::unique_ptr<detail::SchedulerState> pimpl;
};


}