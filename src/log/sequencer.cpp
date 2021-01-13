#include "log/sequencer.h"

#include "utils/bits.h"

namespace faas {
namespace log {

using protocol::SharedLogMessage;
using protocol::SharedLogMessageHelper;
using protocol::SharedLogOpType;

Sequencer::Sequencer(uint16_t node_id)
    : SequencerBase(node_id),
      log_header_(fmt::format("Sequencer[{}-N]: ", node_id)),
      current_view_(nullptr) {}

Sequencer::~Sequencer() {}

void Sequencer::OnViewCreated(const View* view) {
    DCHECK(zk_session()->WithinMyEventLoopThread());
    bool contains_myself = view->contains_sequencer_node(my_node_id());
    std::vector<SharedLogRequest> ready_requests;
    {
        absl::MutexLock core_lk(&core_mu_);
        if (contains_myself) {
            primary_collection_.InstallLogSpace(
                std::make_unique<MetaLogPrimary>(view, my_node_id()));
            for (uint16_t id : view->GetSequencerNodes()) {
                if (view->GetSequencerNode(id)->IsReplicaSequencerNode(my_node_id())) {
                    backup_collection_.InstallLogSpace(
                        std::make_unique<MetaLogBackup>(view, id));
                }
            }
        }
        current_primary_ = primary_collection_.GetLogSpace(
            bits::JoinTwo16(view->id(), my_node_id()));
        DCHECK(!contains_myself || current_primary_ != nullptr);
        {
            absl::MutexLock future_request_lk(&future_request_mu_);
            future_requests_.OnNewView(view, contains_myself ? &ready_requests : nullptr);
        }
        current_view_ = view;
        log_header_ = fmt::format("Sequencer[{}-{}]: ", my_node_id(), view->id());
    }
    if (!ready_requests.empty()) {
        SomeIOWorker()->ScheduleFunction(
            nullptr, [this, requests = std::move(ready_requests)] {
                ProcessRequests(requests);
            }
        );
    }
}

void Sequencer::OnViewFrozen(const View* view) {
    DCHECK(zk_session()->WithinMyEventLoopThread());
    absl::ReaderMutexLock core_lk(&core_mu_);
    // TODO: publish tail metalogs
    DCHECK_EQ(view->id(), current_view_->id());
    if (current_primary_ != nullptr) {
        FreezeLogSpace<MetaLogPrimary>(current_primary_);
    }
    backup_collection_.ForEachActiveLogSpace(
        view,
        [this] (uint32_t, LockablePtr<MetaLogBackup> logspace_ptr) {
            FreezeLogSpace<MetaLogBackup>(std::move(logspace_ptr));
        }
    );
}

void Sequencer::OnViewFinalized(const FinalizedView* finalized_view) {
    DCHECK(zk_session()->WithinMyEventLoopThread());
    absl::ReaderMutexLock core_lk(&core_mu_);
    DCHECK_EQ(finalized_view->view()->id(), current_view_->id());
    if (current_primary_ != nullptr) {
        FinalizedLogSpace<MetaLogPrimary>(current_primary_, finalized_view);
    }
    backup_collection_.ForEachActiveLogSpace(
        finalized_view->view(),
        [finalized_view, this] (uint32_t, LockablePtr<MetaLogBackup> logspace_ptr) {
            FinalizedLogSpace<MetaLogBackup>(std::move(logspace_ptr), finalized_view);
        }
    );
}

#define ONHOLD_IF_FROM_FUTURE_VIEW(MESSAGE_VAR, PAYLOAD_VAR)        \
    do {                                                            \
        if (current_view_ == nullptr                                \
                || (MESSAGE_VAR).view_id > current_view_->id()) {   \
            absl::MutexLock future_request_lk(&future_request_mu_); \
            future_requests_.OnHoldRequest(                         \
                SharedLogRequest(MESSAGE_VAR, PAYLOAD_VAR));        \
            return;                                                 \
        }                                                           \
    } while (0)

#define PANIC_IF_FROM_FUTURE_VIEW(MESSAGE_VAR)                      \
    do {                                                            \
        if (current_view_ == nullptr                                \
                || (MESSAGE_VAR).view_id > current_view_->id()) {   \
            HLOG(FATAL) << "Receive message from future view "      \
                        << (MESSAGE_VAR).view_id;                   \
        }                                                           \
    } while (0)

#define IGNORE_IF_FROM_PAST_VIEW(MESSAGE_VAR)                       \
    do {                                                            \
        if (current_view_ != nullptr                                \
                && (MESSAGE_VAR).view_id < current_view_->id()) {   \
            HLOG(WARNING) << "Receive outdate message from view "   \
                          << (MESSAGE_VAR).view_id;                 \
            return;                                                 \
        }                                                           \
    } while (0)

#define RETURN_IF_LOGSPACE_FROZEN(LOGSPACE_PTR)                     \
    do {                                                            \
        if ((LOGSPACE_PTR)->frozen()) {                             \
            uint32_t logspace_id = (LOGSPACE_PTR)->identifier();    \
            HLOG(WARNING) << fmt::format(                           \
                "LogSpace {} is frozen",                            \
                bits::HexStr0x(logspace_id));                       \
            return;                                                 \
        }                                                           \
    } while (0)

void Sequencer::HandleTrimRequest(const SharedLogMessage& request) {
    DCHECK(SharedLogMessageHelper::GetOpType(request) == SharedLogOpType::TRIM);
    NOT_IMPLEMENTED();
}

void Sequencer::OnRecvMetaLogProgress(const SharedLogMessage& message) {
    DCHECK(SharedLogMessageHelper::GetOpType(message) == SharedLogOpType::META_PROG);
    LockablePtr<MetaLogPrimary> logspace_ptr;
    const View* view = nullptr;
    {
        absl::ReaderMutexLock core_lk(&core_mu_);
        PANIC_IF_FROM_FUTURE_VIEW(message);  // I believe this will never happen
        IGNORE_IF_FROM_PAST_VIEW(message);
        logspace_ptr = primary_collection_.GetLogSpaceChecked(message.logspace_id);
        view = current_view_;
    }
    std::vector<MetaLogProto> new_replicated_metalogs;
    {
        auto locked_logspace = logspace_ptr.Lock();
        RETURN_IF_LOGSPACE_FROZEN(locked_logspace);
        uint32_t old_position = locked_logspace->replicated_metalog_position();
        locked_logspace->UpdateReplicaProgress(
            message.origin_node_id, message.metalog_position);
        uint32_t new_position = locked_logspace->replicated_metalog_position();
        if (new_position > old_position) {
            if (!locked_logspace->GetMetaLogs(old_position, new_position,
                                              &new_replicated_metalogs)) {
                HLOG(FATAL) << fmt::format("Cannot get meta log between {} and {}",
                                           old_position, new_position);
            }
        }
    }
    for (const MetaLogProto& metalog_proto : new_replicated_metalogs) {
        PropagateMetaLog(DCHECK_NOTNULL(view), metalog_proto);
    }
}

void Sequencer::OnRecvShardProgress(const SharedLogMessage& message,
                                    std::span<const char> payload) {
    DCHECK(SharedLogMessageHelper::GetOpType(message) == SharedLogOpType::SHARD_PROG);
    LockablePtr<MetaLogPrimary> logspace_ptr;
    {
        absl::ReaderMutexLock core_lk(&core_mu_);
        ONHOLD_IF_FROM_FUTURE_VIEW(message, payload);
        IGNORE_IF_FROM_PAST_VIEW(message);
        logspace_ptr = primary_collection_.GetLogSpaceChecked(message.logspace_id);
    }
    {
        auto locked_logspace = logspace_ptr.Lock();
        RETURN_IF_LOGSPACE_FROZEN(locked_logspace);
        std::vector<uint32_t> progress(payload.size() / sizeof(uint32_t), 0);
        memcpy(progress.data(), payload.data(), payload.size());
        locked_logspace->UpdateStorageProgress(message.origin_node_id, progress);
    }
}

void Sequencer::OnRecvNewMetaLogs(const SharedLogMessage& message,
                                  std::span<const char> payload) {
    DCHECK(SharedLogMessageHelper::GetOpType(message) == SharedLogOpType::METALOGS);
    uint32_t logspace_id = message.logspace_id;
    MetaLogsProto metalogs_proto = log_utils::MetaLogsFromPayload(payload);
    DCHECK_EQ(metalogs_proto.logspace_id(), logspace_id);
    LockablePtr<MetaLogBackup> logspace_ptr;
    {
        absl::ReaderMutexLock core_lk(&core_mu_);
        ONHOLD_IF_FROM_FUTURE_VIEW(message, payload);
        IGNORE_IF_FROM_PAST_VIEW(message);
        logspace_ptr = backup_collection_.GetLogSpaceChecked(logspace_id);
    }
    uint32_t old_metalog_position;
    uint32_t new_metalog_position;
    {
        auto locked_logspace = logspace_ptr.Lock();
        RETURN_IF_LOGSPACE_FROZEN(locked_logspace);
        old_metalog_position = locked_logspace->metalog_position();
        for (const MetaLogProto& metalog_proto : metalogs_proto.metalogs()) {
            locked_logspace->ProvideMetaLog(metalog_proto);
        }
        new_metalog_position = locked_logspace->metalog_position();
    }
    if (new_metalog_position > old_metalog_position) {
        SharedLogMessage response = SharedLogMessageHelper::NewMetaLogProgressMessage(
            logspace_id, new_metalog_position);
        SendSequencerMessage(message.sequencer_id, &response);
    }
}

#undef ONHOLD_IF_FROM_FUTURE_VIEW
#undef PANIC_IF_FROM_FUTURE_VIEW
#undef IGNORE_IF_FROM_PAST_VIEW

void Sequencer::ProcessRequests(const std::vector<SharedLogRequest>& requests) {
    for (const SharedLogRequest& request : requests) {
        MessageHandler(request.message, STRING_TO_SPAN(request.payload));
    }
}

void Sequencer::MarkNextCutIfDoable() {
    LockablePtr<MetaLogPrimary> logspace_ptr;
    const View* view = nullptr;
    {
        absl::ReaderMutexLock core_lk(&core_mu_);
        logspace_ptr = current_primary_;
        view = current_view_;
    }
    if (logspace_ptr == nullptr || view == nullptr) {
        return;
    }
    MetaLogProto meta_log_proto;
    bool has_new_cut = false;
    {
        auto locked_logspace = logspace_ptr.Lock();
        RETURN_IF_LOGSPACE_FROZEN(locked_logspace);
        if (!locked_logspace->all_metalog_replicated()) {
            HLOG(INFO) << "Not all meta log replicated, will not mark new cut";
            return;
        }
        has_new_cut = locked_logspace->MarkNextCut(&meta_log_proto);
    }
    if (has_new_cut) {
        ReplicateMetaLog(view, meta_log_proto);
    }
}

#undef RETURN_IF_LOGSPACE_FROZEN

}  // namespace log
}  // namespace faas