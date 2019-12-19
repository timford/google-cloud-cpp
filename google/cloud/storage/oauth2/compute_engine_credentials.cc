// Copyright 2019 Google LLC
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

#include "google/cloud/storage/oauth2/compute_engine_credentials.h"
#include "google/cloud/storage/internal/nljson.h"


//struct ServiceAccountMetadata {
//  std::set<std::string> scopes;
//  std::string email;
//};

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace oauth2 {
StatusOr<ServiceAccountMetadata> ParseMetadataServerResponse(
    storage::internal::HttpResponse const& response) {
  LOG(INFO) << "ComputeEngineCredentials ParseMetadataServerResponse() ...";
  LOG(INFO) << "payload is " << response.payload;
  auto response_body =
      storage::internal::nl::json::parse(response.payload, nullptr, false);
  LOG(INFO) << "did finish parsing metadata server response payload...";
  // Note that the "scopes" attribute will always be present and contain a
  // JSON array. At minimum, for the request to succeed, the instance must
  // have been granted the scope that allows it to retrieve info from the
  // metadata server.
  if (response_body.is_discarded() || response_body.count("email") == 0 ||
      response_body.count("scopes") == 0) {
    LOG(INFO) << "issue finding all fields... is_discarded()=" << response_body.is_discarded() << ", email count=" << response_body.count("email") << ", scopes count=" << response_body.count("scopes");
    auto payload =
        response.payload +
        "Could not find all required fields in response (email, scopes).";
    LOG(INFO) << "returning " << payload;
    return AsStatus(storage::internal::HttpResponse{response.status_code,
                                                    payload, response.headers});
  }
  ServiceAccountMetadata metadata;
  // Do not update any state until all potential errors are handled.
  metadata.email = response_body.value("email", "");
  // We need to call the .get<>() helper because the conversion is ambiguous
  // otherwise.
  if (response_body["scopes"].is_array()) {
    metadata.scopes =
        response_body["scopes"].template get<std::set<std::string>>();
  } else {
    metadata.scopes = {response_body["scopes"].get<std::string>()};
  }
  LOG(INFO) << "ComputeEngineCredentials::ParseMetadataServerResponse(): email is" << metadata.email << ", scopes are: ";
  for (auto s : metadata.scopes) {
    LOG(INFO) << "--- scope: " << s;
  }
  LOG(INFO) << "ComputeEngineCredentials::ParseMetadataServerResponse(): returning";
  return metadata;
}

StatusOr<RefreshingCredentialsWrapper::TemporaryToken>
ParseComputeEngineRefreshResponse(
    storage::internal::HttpResponse const& response,
    std::chrono::system_clock::time_point now) {
  LOG(INFO) << "ComputeEngineCredentials::ParseComputeEngineRefreshResponse...";
  namespace nl = storage::internal::nl;
  // Response should have the attributes "access_token", "expires_in", and
  // "token_type".
  nl::json access_token = nl::json::parse(response.payload, nullptr, false);
  LOG(INFO) << "did finish parsing refresh response payload...";
  if (access_token.is_discarded() || access_token.count("access_token") == 0 or
      access_token.count("expires_in") == 0 or
      access_token.count("token_type") == 0) {
    LOG(INFO) << "ComputeEngineCredentials::ParseComputeEngineRefreshResponse problem... is_discarded()=" << access_token.is_discarded() << ", count access token=" << access_token.count("access_token") << ", count expired in=" << access_token.count("expires_in") << ", count token_type=" << access_token.count("token_type");
    auto payload =
        response.payload +
        "Could not find all required fields in response (access_token,"
        " expires_in, token_type).";
    LOG(INFO) << "ComputeEngineCredentials::ParseComputeEngineRefreshResponse returning payload " << payload << " as status";
    return AsStatus(storage::internal::HttpResponse{response.status_code,
                                                    payload, response.headers});
  }
  std::string header = "Authorization: ";
  header += access_token.value("token_type", "");
  header += ' ';
  header += access_token.value("access_token", "");
  LOG(INFO) << "ComputeEngineCredentials::ParseComputeEngineRefreshResponse(): header is " << header;
  auto expires_in =
      std::chrono::seconds(access_token.value("expires_in", int(0)));
  auto new_expiration = now + expires_in;

  LOG(INFO) << "ComputeEngineCredentials::ParseComputeEngineRefreshResponse(): returning temporary token";
  return RefreshingCredentialsWrapper::TemporaryToken{std::move(header),
                                                      new_expiration};
}

}  // namespace oauth2
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
