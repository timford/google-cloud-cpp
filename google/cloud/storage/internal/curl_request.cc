// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "glog/logging.h"
#include "google/cloud/storage/internal/curl_request.h"
#include <iostream>

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {
CurlRequest::CurlRequest() : headers_(nullptr, &curl_slist_free_all) {}

StatusOr<HttpResponse> CurlRequest::MakeRequest(std::string const& payload) {
  LOG(INFO) << "CurlRequest::MakeRequest() with payload \n" << payload;
  if (!payload.empty()) {
    handle_.SetOption(CURLOPT_POSTFIELDSIZE, payload.length());
    handle_.SetOption(CURLOPT_POSTFIELDS, payload.c_str());
  }
  LOG(INFO) << "CurlRequest::MakeRequest() about to call EasyPerform";
  auto status = handle_.EasyPerform();
  LOG(INFO) << "CurlRequest::MakeRequest() done with EasyPerform, status is " << status.message();
  if (!status.ok()) {
    return status;
  }
  handle_.FlushDebug(__func__);
  auto code = handle_.GetResponseCode();
  if (!code.ok()) {
    LOG(INFO) << "CurlRequest::MakeRequest() could not get response code, status is " << code.status().message();
    return std::move(code).status();
  }
  LOG(INFO) << "CurlRequest::MakeRequest() response code is " << code.value();
  LOG(INFO) << "-------------------------------------\n---------------------------------------\nCurlRequest::MakeRequest() returning with response payload:\n" << response_payload_;
  return HttpResponse{code.value(), std::move(response_payload_),
                      std::move(received_headers_)};
}

void CurlRequest::ResetOptions() {
  handle_.SetOption(CURLOPT_URL, url_.c_str());
  handle_.SetOption(CURLOPT_HTTPHEADER, headers_.get());
  handle_.SetOption(CURLOPT_USERAGENT, user_agent_.c_str());
  handle_.SetOption(CURLOPT_NOSIGNAL, 1);
  handle_.SetWriterCallback(
      [this](void* ptr, std::size_t size, std::size_t nmemb) {
        response_payload_.append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
      });
  handle_.SetHeaderCallback([this](char* contents, std::size_t size,
                                   std::size_t nitems) {
    return CurlAppendHeaderData(
        received_headers_, static_cast<char const*>(contents), size * nitems);
  });
  handle_.EnableLogging(logging_enabled_);
  handle_.SetSocketCallback(socket_options_);
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
