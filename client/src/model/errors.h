// Mapping of gateway responses and transport failures to the error taxonomy (client/model-access Typed Error Codes;
// add-agent-runtime design D9) and the retry rules.
#pragma once

#include "base/result.h"
#include "model/types.h"

#include <map>
#include <string>
#include <string_view>

namespace wb::model {

// A failed HTTP response from the gateway. `headers` has lower-case names. The gateway's error body is
// {error:{code, message, reason?, retry_after_s?, reset_at?}}.
ModelFailure failure_from_http(int status, const std::map<std::string, std::string>& headers, std::string_view body);

// A failure of the transport (platform/http.h error codes: connect, timeout, tls, io, cancelled, ...).
ModelFailure failure_from_transport(const Error& error);

// A stream that ended without a finish reason or terminal marker.
ModelFailure truncated_stream_failure(bool partial_output);

// An error object found inside a stream ({"error": {...}}).
ModelFailure failure_from_error_object(const json& error);

// Rejections made by the client itself.
ModelFailure invalid_request(std::string message, std::string reason = {});

}  // namespace wb::model
