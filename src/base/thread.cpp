#include "base/thread.h"

#include "utils/env_variables.h"

#include <sched.h>
#include <sys/syscall.h>

namespace faas {
namespace base {

thread_local Thread* Thread::current_ = nullptr;

namespace {
pid_t gettid() {
    return syscall(SYS_gettid);
}
}

void Thread::Start() {
    state_.store(kStarting);
    CHECK_EQ(pthread_create(&pthread_, nullptr, &Thread::StartRoutine, this), 0);
    started_.WaitForNotification();
    DCHECK(state_.load() == kRunning);
}

void Thread::Join() {
    State state = state_.load();
    if (state == kFinished) {
        return;
    }
    DCHECK(state == kRunning);
    CHECK_EQ(pthread_join(pthread_, nullptr), 0);
}

void Thread::Run() {
    tid_ = gettid();
    state_.store(kRunning);
    started_.Notify();
    LOG(INFO) << "Start thread: " << name_;
    fn_();
    state_.store(kFinished);
}

void Thread::MarkThreadCategory(absl::string_view category) {
    CHECK(current_ == this);
    std::string cpuset_var_name(fmt::format("FAAS_{}_THREAD_CPUSET", category));
    std::string cpuset_str(utils::GetEnvVariable(cpuset_var_name));
    if (!cpuset_str.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (const std::string_view& cpu_str : absl::StrSplit(cpuset_str, ",")) {
            int cpu;
            CHECK(absl::SimpleAtoi(cpu_str, &cpu));
            CPU_SET(cpu, &set);
        }
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            LOG(FATAL) << "Failed to set CPU affinity to " << cpuset_str;
        } else {
            LOG(INFO) << "Successfully set CPU affinity of current thread to " << cpuset_str;
        }
    } else {
        LOG(INFO) << "Does not find cpuset setting for " << category << " threads, "
                  << "use environment variable " << cpuset_var_name << " to set it";
    }
}

void* Thread::StartRoutine(void* arg) {
    Thread* self = reinterpret_cast<Thread*>(arg);
    current_ = self;
    self->Run();
    return nullptr;
}

void Thread::RegisterMainThread() {
    Thread* thread = new Thread("Main", std::function<void()>());
    thread->state_.store(kRunning);
    thread->tid_ = gettid();
    thread->pthread_ = pthread_self();
    current_ = thread;
    LOG(INFO) << "Register main thread: tid=" << thread->tid_;
}

}  // namespace base
}  // namespace faas
