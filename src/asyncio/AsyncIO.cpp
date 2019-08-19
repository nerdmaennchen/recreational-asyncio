#include "AsyncIO.h"

#include <simplyfile/Event.h>

#include <vector>
#include <mutex>
#include <ucontext.h>
#include <exception>

#include <sys/select.h>

namespace asyncio
{

namespace 
{

template<typename Func>
struct Finally {
    Func f;
    Finally(Func&& _f) : f{std::move(_f)} {}
    ~Finally() { f(); }
};

}

namespace sf = simplyfile;

struct TaskInfo {
    using WrappedTask = Scheduler::WrappedTask;
    std::vector<std::byte> stack;
    ucontext_t context;
    
    WrappedTask func;

    simplyfile::Event eventFD{O_NONBLOCK, 1};

    bool doneFlag {false};

    static void runTask(TaskInfo* info, Scheduler* scheduler);

    TaskInfo(std::size_t stack_size, WrappedTask _func, Scheduler* scheduler) 
        : stack{stack_size}
        , func{std::move(_func)} {
        getcontext(&context);
        context.uc_stack.ss_sp = stack.data();
        context.uc_stack.ss_size = stack.size();
        makecontext(&context, reinterpret_cast<void(*)()>(runTask), 2, this, scheduler);
    }
};

struct detail::SchedulerState {
    std::mutex task_infos_mutex;
    std::vector<std::unique_ptr<TaskInfo>> task_infos;

    std::mutex testers_mutex;
    std::vector<std::pair<detail::TesterBase const*, TaskInfo*>> testers;

    void enque_tester(detail::TesterBase const& tester, TaskInfo* info) {
        std::lock_guard lock{testers_mutex};
        testers.emplace_back(&tester, info);
    }

    void deque_tester(detail::TesterBase const& tester) {
        std::lock_guard lock{testers_mutex};
        testers.erase(find_if(begin(testers), end(testers), [&](auto const& other) { return other.first == &tester; }));
    }
};

namespace {

struct StackGuard {
    ucontext_t* callerContext{nullptr};
    detail::SchedulerState* state{nullptr};
    Scheduler* scheduler{nullptr};
    TaskInfo* info{nullptr};
};
thread_local StackGuard stackGuard;

bool stack_guard_valid() {
    return stackGuard.callerContext and stackGuard.state and stackGuard.scheduler and stackGuard.info;
}

}


Scheduler::Scheduler() 
    : Epoll{}, pimpl{std::make_unique<detail::SchedulerState>()}
{}

Scheduler::~Scheduler() {}

void TaskInfo::runTask(TaskInfo* task, Scheduler* scheduler) {
    task->func();
    // auto& pimpl = *scheduler->pimpl;
    scheduler->rmFD(task->eventFD, false);
    // pimpl.tasks.erase(std::remove_if(begin(pimpl.tasks), end(pimpl.tasks), [=](auto const& other) { return other.get() == info; }), end(pimpl.tasks));
    setcontext(stackGuard.callerContext);
}

void Scheduler::enque(WrappedTask task, std::size_t stack_size) {
    auto info = std::make_unique<TaskInfo>(stack_size, std::move(task), this);
    TaskInfo* infoPtr = info.get();
    {
        std::lock_guard lock{pimpl->task_infos_mutex};
        pimpl->task_infos.emplace_back(std::move(info));
    }

    addFD(infoPtr->eventFD, [infoPtr, this](int) {
        infoPtr->eventFD.get();
        StackGuard oldGuard = stackGuard;
        ucontext_t curContext;
        stackGuard = { &curContext, pimpl.get(), this, infoPtr };
        infoPtr->context.uc_link = &curContext;
        swapcontext(&curContext, &infoPtr->context);
        stackGuard = oldGuard;
    }, EPOLLIN|EPOLLET);
}

void Scheduler::work(int maxEvents, int timeout_ms) {
    {
        std::lock_guard lock{pimpl->testers_mutex};
        auto& testers = pimpl->testers;
        auto it = std::partition(begin(testers), end(testers), [](auto const& p){
            return not p.first->test();
        });
        std::for_each(it, end(testers), [](auto const& p){
            p.second->eventFD.put(1);
        });
        testers.erase(it, end(testers));
    }
    simplyfile::Epoll::work(maxEvents, timeout_ms);
}

void detail::await(TesterBase const& tester) {
    if (not stack_guard_valid()) {
        return;
    }
    if (tester.test()) {
        return;
    }
    stackGuard.state->enque_tester(tester, stackGuard.info);
    swapcontext(&stackGuard.info->context, stackGuard.callerContext);
}

std::future<void> readable(int fd) {
    if (not stack_guard_valid()) {
        return std::async(std::launch::deferred, [=](){
            fd_set fdset; 
            FD_ZERO(&fdset); 
            FD_SET(fd, &fdset);
            select(1, &fdset, nullptr, nullptr, nullptr);
        });
    }
    auto promise = std::make_shared<std::promise<void>>();
    stackGuard.scheduler->addFD(fd, [=, sched=stackGuard.scheduler](int events) {
        sched->rmFD(fd, false);
        if (events & EPOLLERR) {
            try {
                throw std::runtime_error("cannot wait for fd to become readable");
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        } else {
            promise->set_value();
        }
    }, EPOLLIN|EPOLLET);
    return promise->get_future();
}

std::future<void> writable(int fd) {
    if (not stack_guard_valid()) {
        return std::async(std::launch::deferred, [=](){
            fd_set fdset; 
            FD_ZERO(&fdset); 
            FD_SET(fd, &fdset);
            select(1, nullptr, &fdset, nullptr, nullptr);
        });
    }
    auto promise = std::make_shared<std::promise<void>>();
    stackGuard.scheduler->addFD(fd, [=, sched=stackGuard.scheduler](int events) {
        sched->rmFD(fd, false);
        if (events & EPOLLERR) {
            try {
                throw std::runtime_error("cannot wait for fd to become writable");
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        } else {
            promise->set_value();
        }
    }, EPOLLOUT|EPOLLET);
    return promise->get_future();
}

}
