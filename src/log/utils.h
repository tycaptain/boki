#pragma once

#include "log/common.h"
#include "log/view.h"

namespace faas {
namespace log_utils {

// Used for on-holding requests for future views
class FutureRequests {
public:
    FutureRequests();
    ~FutureRequests();

    // If `ready_requests` is nullptr, will panic if there are on-hold requests
    void OnNewView(const log::View* view,
                   std::vector<log::SharedLogRequest>* ready_requests);
    void OnHoldRequest(log::SharedLogRequest request);

private:
    uint16_t next_view_id_;
    absl::flat_hash_map</* view_id */ uint16_t, std::vector<log::SharedLogRequest>>
        onhold_requests_;

    DISALLOW_COPY_AND_ASSIGN(FutureRequests);
};

log::MetaLogsProto MetaLogsFromPayload(std::span<const char> payload);

void PopulateMetaDataFromRequest(const protocol::SharedLogMessage& request,
                                 log::LogMetaData* metadata);
void PopulateMetaDataToResponse(const log::LogMetaData& metadata,
                                protocol::SharedLogMessage* response);
void PopulateMetaDataToResponse(const log::LogEntryProto& log_entry,
                                protocol::SharedLogMessage* response);

}  // namespace log_utils
}  // namespace faas