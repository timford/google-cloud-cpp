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

#include "google/cloud/storage/oauth2/google_credentials.h"
#include "google/cloud/internal/filesystem.h"
#include "google/cloud/internal/make_unique.h"
#include "google/cloud/internal/throw_delegate.h"
#include "google/cloud/storage/internal/nljson.h"
#include "google/cloud/storage/oauth2/anonymous_credentials.h"
#include "google/cloud/storage/oauth2/authorized_user_credentials.h"
#include "google/cloud/storage/oauth2/compute_engine_credentials.h"
#include "google/cloud/storage/oauth2/google_application_default_credentials_file.h"
#include "google/cloud/storage/oauth2/service_account_credentials.h"
#include <fstream>
#include <iterator>
#include <memory>
#include "glog/logging.h"

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace oauth2 {

constexpr char kAdcLink[] =
    "https://developers.google.com/identity/protocols/"
    "application-default-credentials";

// Parses the JSON or P12 file at `path` and creates the appropriate
// Credentials type.
//
// If `service_account_scopes` or `service_account_subject` are specified, the
// file at `path` must be a P12 service account or a JSON service account. If
// a different type of credential file is found, this function returns
// nullptr to indicate a service account file wasn't found.
StatusOr<std::unique_ptr<Credentials>> LoadCredsFromPath(
    std::string const& path, bool non_service_account_ok,
    google::cloud::optional<std::set<std::string>> service_account_scopes,
    google::cloud::optional<std::string> service_account_subject) {
  namespace nl = google::cloud::storage::internal::nl;

  std::ifstream ifs(path);
  if (!ifs.is_open()) {
    // We use kUnknown here because we don't know if the file does not exist, or
    // if we were unable to open it for some other reason.
    return Status(StatusCode::kUnknown, "Cannot open credentials file " + path);
  }
  std::string contents(std::istreambuf_iterator<char>{ifs}, {});
  auto cred_json = nl::json::parse(contents, nullptr, false);
  if (cred_json.is_discarded()) {
    // This is not a JSON file, try to load it as a P12 service account.
    auto info = ParseServiceAccountP12File(path);
    if (!info) {
      // Ignore the error returned by the P12 parser, because those are too
      // specific, they typically say "error in PKCS#12" and the application
      // may not even be trying to load a PKCS#12 file.
      return Status(StatusCode::kInvalidArgument,
                    "Invalid credentials file " + path);
    }
    info->subject = std::move(service_account_subject);
    info->scopes = std::move(service_account_scopes);
    auto credentials =
        google::cloud::internal::make_unique<ServiceAccountCredentials<>>(
            *info);
    return std::unique_ptr<Credentials>(std::move(credentials));
  }
  std::string cred_type = cred_json.value("type", "no type given");
  // If non_service_account_ok==false and the cred_type is authorized_user,
  // we'll return "Unsupported credential type (authorized_user)".
  if (cred_type == "authorized_user" && non_service_account_ok) {
    if (service_account_scopes || service_account_subject) {
      // No ptr indicates that the file we found was not a service account file.
      return StatusOr<std::unique_ptr<Credentials>>(nullptr);
    }
    auto info = ParseAuthorizedUserCredentials(contents, path);
    if (!info) {
      return info.status();
    }
    std::unique_ptr<Credentials> ptr =
        google::cloud::internal::make_unique<AuthorizedUserCredentials<>>(
            *info);
    return StatusOr<std::unique_ptr<Credentials>>(std::move(ptr));
  }
  if (cred_type == "service_account") {
    auto info = ParseServiceAccountCredentials(contents, path);
    if (!info) {
      return info.status();
    }
    info->subject = std::move(service_account_subject);
    info->scopes = std::move(service_account_scopes);
    std::unique_ptr<Credentials> ptr =
        google::cloud::internal::make_unique<ServiceAccountCredentials<>>(
            *info);
    return StatusOr<std::unique_ptr<Credentials>>(std::move(ptr));
  }
  return StatusOr<std::unique_ptr<Credentials>>(
      Status(StatusCode::kInvalidArgument,
             "Unsupported credential type (" + cred_type +
                 ") when reading Application Default Credentials file from " +
                 path + "."));
}

// Tries to load the file at the path specified by the value of the Application
// Default %Credentials environment variable and to create the appropriate
// Credentials type.
//
// Returns nullptr if the environment variable is not set or the path does not
// exist.
//
// If `service_account_scopes` or `service_account_subject` are specified, the
// found file must be a P12 service account or a JSON service account. If a
// different type of credential file is found, this function returns nullptr
// to indicate a service account file wasn't found.
StatusOr<std::unique_ptr<Credentials>> MaybeLoadCredsFromAdcPaths(
    bool non_service_account_ok,
    google::cloud::optional<std::set<std::string>> service_account_scopes,
    google::cloud::optional<std::string> service_account_subject) {
  // 1) Check if the GOOGLE_APPLICATION_CREDENTIALS environment variable is set.
  auto path = GoogleAdcFilePathFromEnvVarOrEmpty();
  if (path.empty()) {
    // 2) If no path was specified via environment variable, check if the
    // gcloud ADC file exists.
    path = GoogleAdcFilePathFromWellKnownPathOrEmpty();
    if (path.empty()) {
      return StatusOr<std::unique_ptr<Credentials>>(nullptr);
    }
    // Just because we had the necessary information to build the path doesn't
    // mean that a file exists there.
    std::error_code ec;
    auto adc_file_status = google::cloud::internal::status(path, ec);
    if (!google::cloud::internal::exists(adc_file_status)) {
      return StatusOr<std::unique_ptr<Credentials>>(nullptr);
    }
  }

  // If the path was specified, try to load that file; explicitly fail if it
  // doesn't exist or can't be read and parsed.
  return LoadCredsFromPath(path, non_service_account_ok,
                           std::move(service_account_scopes),
                           std::move(service_account_subject));
}

StatusOr<std::shared_ptr<Credentials>> GoogleDefaultCredentials() {
  // 1 and 2) Check if the GOOGLE_APPLICATION_CREDENTIALS environment variable
  // is set or if the gcloud ADC file exists.
  LOG(INFO) << "About to call MaybeLoadCredsFromAdcPaths";
  auto creds = MaybeLoadCredsFromAdcPaths(true, {}, {});
  if (!creds) {
    LOG(INFO) << "Error with status: " << creds.status().message();
    return StatusOr<std::shared_ptr<Credentials>>(creds.status());
  }
  if (*creds) {
    LOG(INFO) << "MaybeLoadCredsFromAdcPaths successful, returning creds.";
    return StatusOr<std::shared_ptr<Credentials>>(std::move(*creds));
  }
  LOG(INFO) << "MaybeLoadCredsFromAdcPaths not successful. Continuing.";

  // 3) Check for implicit environment-based credentials (GCE, GAE Flexible
  // Environment).
  // Note: GCE credentials *should* also work when running on a VM instance in
  // the App Engine Flexible Environment, but this has not been explicitly
  // tested, as it requires a custom GAEF runtime.
  if (storage::internal::RunningOnComputeEngineVm()) {
    LOG(INFO) << "Running on GCE VM. Building and returning GCE credentials.";
    return StatusOr<std::shared_ptr<Credentials>>(
        std::make_shared<ComputeEngineCredentials<>>());
  }
  LOG(INFO) << "Not running on GCE VM. Returning failure.";

  // We've exhausted all search points, thus credentials cannot be constructed.
  return StatusOr<std::shared_ptr<Credentials>>(
      Status(StatusCode::kUnknown,
             "Could not automatically determine credentials. For more "
             "information, please see " +
                 std::string(kAdcLink)));
}

std::shared_ptr<Credentials> CreateAnonymousCredentials() {
  return std::make_shared<AnonymousCredentials>();
}

StatusOr<std::shared_ptr<Credentials>>
CreateAuthorizedUserCredentialsFromJsonFilePath(std::string const& path) {
  std::ifstream is(path);
  std::string contents(std::istreambuf_iterator<char>{is}, {});
  auto info = ParseAuthorizedUserCredentials(contents, path);
  if (!info) {
    return StatusOr<std::shared_ptr<Credentials>>(info.status());
  }
  return StatusOr<std::shared_ptr<Credentials>>(
      std::make_shared<AuthorizedUserCredentials<>>(*info));
}

StatusOr<std::shared_ptr<Credentials>>
CreateAuthorizedUserCredentialsFromJsonContents(std::string const& contents) {
  auto info = ParseAuthorizedUserCredentials(contents, "memory");
  if (!info) {
    return StatusOr<std::shared_ptr<Credentials>>(info.status());
  }
  return StatusOr<std::shared_ptr<Credentials>>(
      std::make_shared<AuthorizedUserCredentials<>>(*info));
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromFilePath(std::string const& path) {
  return CreateServiceAccountCredentialsFromFilePath(path, {}, {});
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromFilePath(
    std::string const& path,
    google::cloud::optional<std::set<std::string>> scopes,
    google::cloud::optional<std::string> subject) {
  auto credentials =
      CreateServiceAccountCredentialsFromJsonFilePath(path, scopes, subject);
  if (credentials) {
    return credentials;
  }
  return CreateServiceAccountCredentialsFromP12FilePath(path, std::move(scopes),
                                                        std::move(subject));
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromJsonFilePath(std::string const& path) {
  return CreateServiceAccountCredentialsFromJsonFilePath(path, {}, {});
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromJsonFilePath(
    std::string const& path,
    google::cloud::optional<std::set<std::string>> scopes,
    google::cloud::optional<std::string> subject) {
  std::ifstream is(path);
  std::string contents(std::istreambuf_iterator<char>{is}, {});
  auto info = ParseServiceAccountCredentials(contents, path);
  if (!info) {
    return StatusOr<std::shared_ptr<Credentials>>(info.status());
  }
  // These are supplied as extra parameters to this method, not in the JSON
  // file.
  info->subject = std::move(subject);
  info->scopes = std::move(scopes);
  return StatusOr<std::shared_ptr<Credentials>>(
      std::make_shared<ServiceAccountCredentials<>>(*info));
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromP12FilePath(
    std::string const& path,
    google::cloud::optional<std::set<std::string>> scopes,
    google::cloud::optional<std::string> subject) {
  auto info = ParseServiceAccountP12File(path);
  if (!info) {
    return StatusOr<std::shared_ptr<Credentials>>(info.status());
  }
  // These are supplied as extra parameters to this method, not in the P12
  // file.
  info->subject = std::move(subject);
  info->scopes = std::move(scopes);
  return StatusOr<std::shared_ptr<Credentials>>(
      std::make_shared<ServiceAccountCredentials<>>(*info));
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromP12FilePath(std::string const& path) {
  return CreateServiceAccountCredentialsFromP12FilePath(path, {}, {});
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromDefaultPaths() {
  return CreateServiceAccountCredentialsFromDefaultPaths({}, {});
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromDefaultPaths(
    google::cloud::optional<std::set<std::string>> scopes,
    google::cloud::optional<std::string> subject) {
  auto creds =
      MaybeLoadCredsFromAdcPaths(false, std::move(scopes), std::move(subject));
  if (!creds) {
    return StatusOr<std::shared_ptr<Credentials>>(creds.status());
  }
  if (*creds) {
    return StatusOr<std::shared_ptr<Credentials>>(std::move(*creds));
  }

  // We've exhausted all search points, thus credentials cannot be constructed.
  return StatusOr<std::shared_ptr<Credentials>>(
      Status(StatusCode::kUnknown,
             "Could not create service account credentials using Application"
             "Default Credentials paths. For more information, please see " +
                 std::string(kAdcLink)));
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromJsonContents(std::string const& contents) {
  return CreateServiceAccountCredentialsFromJsonContents(contents, {}, {});
}

StatusOr<std::shared_ptr<Credentials>>
CreateServiceAccountCredentialsFromJsonContents(
    std::string const& contents,
    google::cloud::optional<std::set<std::string>> scopes,
    google::cloud::optional<std::string> subject) {
  auto info = ParseServiceAccountCredentials(contents, "memory");
  if (!info) {
    return StatusOr<std::shared_ptr<Credentials>>(info.status());
  }
  // These are supplied as extra parameters to this method, not in the JSON
  // file.
  info->subject = std::move(subject);
  info->scopes = std::move(scopes);
  return StatusOr<std::shared_ptr<Credentials>>(
      std::make_shared<ServiceAccountCredentials<>>(*info));
}

std::shared_ptr<Credentials> CreateComputeEngineCredentials() {
  return std::make_shared<ComputeEngineCredentials<>>();
}

std::shared_ptr<Credentials> CreateComputeEngineCredentials(
    std::string const& service_account_email) {
  return std::make_shared<ComputeEngineCredentials<>>(service_account_email);
}

}  // namespace oauth2
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
