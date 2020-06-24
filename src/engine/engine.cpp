#include "engine/engine.h"

#include "ipc/base.h"
#include "ipc/shm_region.h"
#include "common/time.h"
#include "utils/fs.h"
#include "utils/io.h"
#include "utils/docker.h"
#include "worker/worker_lib.h"

#include <absl/flags/flag.h>

ABSL_FLAG(bool, disable_monitor, false, "");

#define HLOG(l) LOG(l) << "Engine: "
#define HVLOG(l) VLOG(l) << "Engine: "

namespace faas {
namespace engine {

using protocol::FuncCall;
using protocol::FuncCallDebugString;
using protocol::Message;
using protocol::GetFuncCallFromMessage;
using protocol::GetInlineDataFromMessage;
using protocol::IsLauncherHandshakeMessage;
using protocol::IsFuncWorkerHandshakeMessage;
using protocol::IsInvokeFuncMessage;
using protocol::IsFuncCallCompleteMessage;
using protocol::IsFuncCallFailedMessage;
using protocol::NewHandshakeResponseMessage;
using protocol::ComputeMessageDelay;

Engine::Engine()
    : gateway_port_(-1),
      listen_backlog_(kDefaultListenBackLog), num_io_workers_(kDefaultNumIOWorkers),
      next_gateway_conn_worker_id_(0), next_ipc_conn_worker_id_(0),
      worker_manager_(new WorkerManager(this)),
      monitor_(absl::GetFlag(FLAGS_disable_monitor) ? nullptr : new Monitor(this)),
      tracer_(new Tracer(this)),
      inflight_external_requests_(0),
      last_external_request_timestamp_(-1),
      incoming_external_requests_stat_(
          stat::Counter::StandardReportCallback("incoming_external_requests")),
      external_requests_instant_rps_stat_(
          stat::StatisticsCollector<float>::StandardReportCallback("external_requests_instant_rps")),
      inflight_external_requests_stat_(
          stat::StatisticsCollector<uint16_t>::StandardReportCallback("inflight_external_requests")),
      message_delay_stat_(
          stat::StatisticsCollector<int32_t>::StandardReportCallback("message_delay")),
      input_use_shm_stat_(stat::Counter::StandardReportCallback("input_use_shm")),
      output_use_shm_stat_(stat::Counter::StandardReportCallback("output_use_shm")),
      discarded_func_call_stat_(stat::Counter::StandardReportCallback("discarded_func_call")) {
    UV_DCHECK_OK(uv_pipe_init(uv_loop(), &uv_ipc_handle_, 0));
    uv_ipc_handle_.data = this;
}

Engine::~Engine() {}

void Engine::StartInternal() {
    // Load function config file
    CHECK(!func_config_file_.empty());
    CHECK(fs_utils::ReadContents(func_config_file_, &func_config_json_))
        << "Failed to read from file " << func_config_file_;
    CHECK(func_config_.Load(func_config_json_));
    // Start IO workers
    CHECK_GT(num_io_workers_, 0);
    HLOG(INFO) << "Start " << num_io_workers_
               << " IO workers for both HTTP and IPC connections";
    for (int i = 0; i < num_io_workers_; i++) {
        auto io_worker = CreateIOWorker(absl::StrFormat("IO-%d", i));
        io_workers_.push_back(io_worker);
    }
    // Listen on ipc_path
    std::string ipc_path(ipc::GetEngineUnixSocketPath());
    if (fs_utils::Exists(ipc_path)) {
        PCHECK(fs_utils::Remove(ipc_path));
    }
    UV_CHECK_OK(uv_pipe_bind(&uv_ipc_handle_, ipc_path.c_str()));
    HLOG(INFO) << "Listen on " << ipc_path << " for IPC connections";
    UV_CHECK_OK(uv_listen(
        UV_AS_STREAM(&uv_ipc_handle_), listen_backlog_,
        &Engine::MessageConnectionCallback));
    // Initialize tracer
    tracer_->Init();
}

void Engine::StopInternal() {
    uv_close(UV_AS_HANDLE(&uv_ipc_handle_), nullptr);
}

void Engine::OnConnectionClose(server::ConnectionBase* connection) {
    DCHECK_IN_EVENT_LOOP_THREAD(uv_loop());
    if (connection->type() == MessageConnection::kTypeId) {
        DCHECK(message_connections_.contains(connection->id()));
        MessageConnection* message_connection = static_cast<MessageConnection*>(connection);
        if (message_connection->handshake_done()) {
            if (message_connection->is_launcher_connection()) {
                worker_manager_->OnLauncherDisconnected(message_connection);
            } else {
                worker_manager_->OnFuncWorkerDisconnected(message_connection);
            }
        }
        message_connections_.erase(connection->id());
        HLOG(INFO) << "A MessageConnection is returned";
    } else {
        HLOG(ERROR) << "Unknown connection type!";
    }
}

bool Engine::OnNewHandshake(MessageConnection* connection,
                            const Message& handshake_message, Message* response,
                            std::span<const char>* response_payload) {
    if (!IsLauncherHandshakeMessage(handshake_message)
          && !IsFuncWorkerHandshakeMessage(handshake_message)) {
        HLOG(ERROR) << "Received message is not a handshake message";
        return false;
    }
    HLOG(INFO) << "Receive new handshake message from message connection";
    uint16_t func_id = handshake_message.func_id;
    if (func_config_.find_by_func_id(func_id) == nullptr) {
        HLOG(ERROR) << "Invalid func_id " << func_id << " in handshake message";
        return false;
    }
    bool success;
    if (IsLauncherHandshakeMessage(handshake_message)) {
        std::span<const char> payload = GetInlineDataFromMessage(handshake_message);
        if (payload.size() != docker_utils::kContainerIdLength) {
            HLOG(ERROR) << "Launcher handshake does not have container ID in inline data";
            return false;
        }
        std::string container_id(payload.data(), payload.size());
        if (monitor_ != nullptr && container_id != docker_utils::kInvalidContainerId) {
            monitor_->OnNewFuncContainer(func_id, container_id);
        }
        success = worker_manager_->OnLauncherConnected(connection);
    } else {
        success = worker_manager_->OnFuncWorkerConnected(connection);
        ProcessDiscardedFuncCallIfNecessary();
    }
    if (!success) {
        return false;
    }
    *response = NewHandshakeResponseMessage(func_config_json_.size());
    *response_payload = std::span<const char>(func_config_json_.data(), func_config_json_.size());
    return true;
}

void Engine::OnRecvMessage(MessageConnection* connection, const Message& message) {
    int32_t message_delay = ComputeMessageDelay(message);
    if (IsInvokeFuncMessage(message)) {
        FuncCall func_call = GetFuncCallFromMessage(message);
        FuncCall parent_func_call;
        parent_func_call.full_call_id = message.parent_call_id;
        Dispatcher* dispatcher = nullptr;
        {
            absl::MutexLock lk(&mu_);
            if (message.payload_size < 0) {
                input_use_shm_stat_.Tick();
            }
            if (message_delay >= 0) {
                message_delay_stat_.AddSample(message_delay);
            }
            dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
        }
        bool success = false;
        if (dispatcher != nullptr) {
            if (message.payload_size < 0) {
                success = dispatcher->OnNewFuncCall(
                    func_call, parent_func_call,
                    /* input_size= */ gsl::narrow_cast<size_t>(-message.payload_size),
                    std::span<const char>(), /* shm_input= */ true);
                
            } else {
                success = dispatcher->OnNewFuncCall(
                    func_call, parent_func_call,
                    /* input_size= */ gsl::narrow_cast<size_t>(message.payload_size),
                    GetInlineDataFromMessage(message), /* shm_input= */ false);
            }
        }
        if (!success) {
            HLOG(ERROR) << "Dispatcher failed for func_id " << func_call.func_id;
        }
    } else if (IsFuncCallCompleteMessage(message) || IsFuncCallFailedMessage(message)) {
        FuncCall func_call = GetFuncCallFromMessage(message);
        Dispatcher* dispatcher = nullptr;
        std::unique_ptr<ipc::ShmRegion> input_region = nullptr;
        {
            absl::MutexLock lk(&mu_);
            if (message_delay >= 0) {
                message_delay_stat_.AddSample(message_delay);
            }
            if (IsFuncCallCompleteMessage(message)) {
                if ((func_call.client_id == 0 && message.payload_size < 0)
                      || (func_call.client_id > 0
                          && message.payload_size + sizeof(int32_t) > PIPE_BUF)) {
                    output_use_shm_stat_.Tick();
                }
            }
            if (func_call.client_id == 0) {
                input_region = GrabExternalFuncCallShmInput(func_call);
            }
            dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
        }
        if (dispatcher != nullptr) {
            if (IsFuncCallCompleteMessage(message)) {
                bool success = dispatcher->OnFuncCallCompleted(
                    func_call, message.processing_time, message.dispatch_delay,
                    /* output_size= */ gsl::narrow_cast<size_t>(std::abs(message.payload_size)));
                if (success && func_call.client_id == 0) {
                    if (message.payload_size < 0) {
                        auto output_region = ipc::ShmOpen(
                            ipc::GetFuncCallOutputShmName(func_call.full_call_id));
                        if (output_region == nullptr) {
                            ExternalFuncCallFinished(
                                func_call, /* success= */ false, /* discarded= */ false,
                                /* output= */ std::span<const char>());
                        } else {
                            output_region->EnableRemoveOnDestruction();
                            ExternalFuncCallFinished(
                                func_call, /* success= */ true, /* discarded= */ false,
                                /* output= */ output_region->to_span());
                        }
                    } else {
                        ExternalFuncCallFinished(
                            func_call, /* success= */ true, /* discarded= */ false,
                            /* output= */ GetInlineDataFromMessage(message));
                    }
                }
            } else {
                bool success = dispatcher->OnFuncCallFailed(func_call, message.dispatch_delay);
                if (success && func_call.client_id == 0) {
                    ExternalFuncCallFinished(
                        func_call, /* success= */ false, /* discarded= */ false,
                        /* output= */ std::span<const char>());
                }
            }
        }
    } else {
        LOG(ERROR) << "Unknown message type!";
    }
    ProcessDiscardedFuncCallIfNecessary();
}

void Engine::OnExternalFuncCall(const FuncCall& func_call, std::span<const char> input) {
    inflight_external_requests_.fetch_add(1);
    std::unique_ptr<ipc::ShmRegion> input_region = nullptr;
    if (input.size() > MESSAGE_INLINE_DATA_SIZE) {
        input_region = ipc::ShmCreate(
            ipc::GetFuncCallInputShmName(func_call.full_call_id), input.size());
        if (input_region == nullptr) {
            ExternalFuncCallFinished(
                func_call, /* success= */ false, /* discarded= */ false,
                /* output= */ std::span<const char>());
            return;
        }
        input_region->EnableRemoveOnDestruction();
        if (input.size() > 0) {
            memcpy(input_region->base(), input.data(), input.size());
        }
    }
    Dispatcher* dispatcher = nullptr;
    {
        absl::MutexLock lk(&mu_);
        incoming_external_requests_stat_.Tick();
        int64_t current_timestamp = GetMonotonicMicroTimestamp();
        if (last_external_request_timestamp_ != -1) {
            external_requests_instant_rps_stat_.AddSample(gsl::narrow_cast<float>(
                1e6 / (current_timestamp - last_external_request_timestamp_)));
        }
        last_external_request_timestamp_ = current_timestamp;
        inflight_external_requests_stat_.AddSample(
            gsl::narrow_cast<uint16_t>(inflight_external_requests_.load()));
        dispatcher = GetOrCreateDispatcherLocked(func_call.func_id);
        if (input_region != nullptr) {
            if (dispatcher != nullptr) {
                external_func_call_shm_inputs_[func_call.full_call_id] = std::move(input_region);
            }
            input_use_shm_stat_.Tick();
        }
    }
    if (dispatcher == nullptr) {
        ExternalFuncCallFinished(
            func_call, /* success= */ false, /* discarded= */ false,
            /* output= */ std::span<const char>());
        return;
    }
    bool success = false;
    if (input.size() <= MESSAGE_INLINE_DATA_SIZE) {
        success = dispatcher->OnNewFuncCall(
            func_call, protocol::kInvalidFuncCall,
            input.size(), /* inline_input= */ input, /* shm_input= */ false);
    } else {
        success = dispatcher->OnNewFuncCall(
            func_call, protocol::kInvalidFuncCall,
            input.size(), /* inline_input= */ std::span<const char>(), /* shm_input= */ true);
    }
    if (!success) {
        {
            absl::MutexLock lk(&mu_);
            input_region = GrabExternalFuncCallShmInput(func_call);
        }
        ExternalFuncCallFinished(
            func_call, /* success= */ false, /* discarded= */ false,
            /* output= */ std::span<const char>());
    }
}

void Engine::ExternalFuncCallFinished(const FuncCall& func_call, bool success, bool discarded,
                                      std::span<const char> output, int status_code) {

}

Dispatcher* Engine::GetOrCreateDispatcher(uint16_t func_id) {
    absl::MutexLock lk(&mu_);
    Dispatcher* dispatcher = GetOrCreateDispatcherLocked(func_id);
    return dispatcher;
}

Dispatcher* Engine::GetOrCreateDispatcherLocked(uint16_t func_id) {
    if (dispatchers_.contains(func_id)) {
        return dispatchers_[func_id].get();
    }
    if (func_config_.find_by_func_id(func_id) != nullptr) {
        dispatchers_[func_id] = std::make_unique<Dispatcher>(this, func_id);
        return dispatchers_[func_id].get();
    } else {
        return nullptr;
    }
}

std::unique_ptr<ipc::ShmRegion> Engine::GrabExternalFuncCallShmInput(const FuncCall& func_call) {
    std::unique_ptr<ipc::ShmRegion> ret = nullptr;
    if (external_func_call_shm_inputs_.contains(func_call.full_call_id)) {
        ret = std::move(external_func_call_shm_inputs_[func_call.full_call_id]);
        external_func_call_shm_inputs_.erase(func_call.full_call_id);
    }
    return ret;
}

void Engine::DiscardFuncCall(const FuncCall& func_call) {
    absl::MutexLock lk(&mu_);
    discarded_func_calls_.push_back(func_call);
    discarded_func_call_stat_.Tick();
}

void Engine::ProcessDiscardedFuncCallIfNecessary() {
    std::vector<std::unique_ptr<ipc::ShmRegion>> discarded_input_regions;
    std::vector<FuncCall> discarded_external_func_calls;
    std::vector<FuncCall> discarded_internal_func_calls;
    {
        absl::MutexLock lk(&mu_);
        for (const FuncCall& func_call : discarded_func_calls_) {
            if (func_call.client_id == 0) {
                auto shm_input = GrabExternalFuncCallShmInput(func_call);
                if (shm_input != nullptr) {
                    discarded_input_regions.push_back(std::move(shm_input));
                }
                discarded_external_func_calls.push_back(func_call);
            } else {
                discarded_internal_func_calls.push_back(func_call);
            }
        }
        discarded_func_calls_.clear();
    }
    for (const FuncCall& func_call : discarded_external_func_calls) {
        ExternalFuncCallFinished(
            func_call, /* success= */ false, /* discarded= */ true,
            /* output= */ std::span<const char>());
    }
    if (!discarded_internal_func_calls.empty()) {
        char pipe_buf[PIPE_BUF];
        Message dummy_message;
        for (const FuncCall& func_call : discarded_internal_func_calls) {
            worker_lib::FuncCallFinished(
                func_call, /* success= */ false, /* output= */ std::span<const char>(),
                /* processing_time= */ 0, pipe_buf, &dummy_message);
        }
    }
}

UV_CONNECTION_CB_FOR_CLASS(Engine, MessageConnection) {
    if (status != 0) {
        HLOG(WARNING) << "Failed to open message connection: " << uv_strerror(status);
        return;
    }
    HLOG(INFO) << "New message connection";
    std::shared_ptr<server::ConnectionBase> connection(new MessageConnection(this));
    uv_pipe_t* client = reinterpret_cast<uv_pipe_t*>(malloc(sizeof(uv_pipe_t)));
    UV_DCHECK_OK(uv_pipe_init(uv_loop(), client, 0));
    if (uv_accept(UV_AS_STREAM(&uv_ipc_handle_), UV_AS_STREAM(client)) == 0) {
        DCHECK_LT(next_ipc_conn_worker_id_, io_workers_.size());
        server::IOWorker* io_worker = io_workers_[next_ipc_conn_worker_id_];
        next_ipc_conn_worker_id_ = (next_ipc_conn_worker_id_ + 1) % io_workers_.size();
        RegisterConnection(io_worker, connection.get(), UV_AS_STREAM(client));
        DCHECK_GE(connection->id(), 0);
        DCHECK(!message_connections_.contains(connection->id()));
        message_connections_[connection->id()] = std::move(connection);
    } else {
        LOG(ERROR) << "Failed to accept new message connection";
        free(client);
    }
}

}  // namespace engine
}  // namespace faas
