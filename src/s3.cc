#include "s3.h"

#include <aws/core/Aws.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <pxr/base/tf/diagnosticLite.h>
#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <time.h>

#include <boost/asio/async_result.hpp>
#include <fstream>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

// -------------------------------------------------------------------------------
// If you want to print out a stacktrace everywhere S3_WARN is called, set this
// to a value > 0 - it will print out this number of stacktrace entries
#define USD_S3_DEBUG_STACKTRACE_SIZE 0

#if USD_S3_DEBUG_STACKTRACE_SIZE > 0

#include <execinfo.h>

#define S3_WARN                                                       \
  {                                                                   \
    void* backtrace_array[USD_S3_DEBUG_STACKTRACE_SIZE];              \
    size_t stack_size =                                               \
        backtrace(backtrace_array, USD_S3_DEBUG_STACKTRACE_SIZE);     \
    TF_WARN("\n\n====================================\n");            \
    TF_WARN("Stacktrace:\n");                                         \
    backtrace_symbols_fd(backtrace_array, stack_size, STDERR_FILENO); \
  }                                                                   \
  TF_WARN

#else  // STACKTRACE_SIZE

#define S3_WARN TF_WARN

#endif  // STACKTRACE_SIZE

// -------------------------------------------------------------------------------

// If you want to control the number of seconds an idle connection is kept alive
// for, set this to something other than zero

#define SESSION_WAIT_TIMEOUT 0

#if SESSION_WAIT_TIMEOUT > 0

#define _USD_S3_SIMPLE_QUOTE(ARG) #ARG
#define _USD_S3_EXPAND_AND_QUOTE(ARG) _SIMPLE_QUOTE(ARG)
#define SET_SESSION_WAIT_TIMEOUT_QUERY \
  ("SET SESSION wait_timeout=" _USD_S3_EXPAND_AND_QUOTE(SESSION_WAIT_TIMEOUT))
#define SET_SESSION_WAIT_TIMEOUT_QUERY_STRLEN \
  (sizeof(SET_SESSION_WAIT_TIMEOUT_QUERY) - 1)

#endif  // SESSION_WAIT_TIMEOUT

// -------------------------------------------------------------------------------

namespace {

constexpr double INVALID_TIME = std::numeric_limits<double>::lowest();

// using mutex_scoped_lock = std::lock_guard<std::mutex>;

// Otherwise clang static analyser will throw errors.
template <size_t len>
constexpr size_t cexpr_strlen(const char (&)[len]) {
  return len - 1;
}

// Parse an S3 url and strip off the prefix ('s3:', 's3:/' or 's3://')
// e.g. s3://bucket/object.usd returns bucket/object.usd
std::string ParsePath(const std::string& path) {
  constexpr auto schema_length_short = cexpr_strlen(usd_s3::S3_PREFIX_SHORT);
  return path.substr(path.find_first_not_of("/", schema_length_short));
}

// Get the bucket from a parsed path
// e.g. 'bucket/object.usd' returns 'bucket'
//      'bucket/somedir/object.usd' returns 'bucket'
const std::string GetBucketName(const std::string& path) {
  return path.substr(0, path.find_first_of('/'));
}

// Get the object from a parsed path
// e.g. 'bucket/object.usd' returns 'object.usd'
//      'bucket/somedir/object.usd' returns 'somedir/object.usd'
//      'bucket/object.usd?versionId=abc123' returns object.usd
const std::string GetObjectName(const std::string& path) {
  const int i = path.find_first_of('/');
  return path.substr(i, path.find_first_of('?') - i);
}

// Check if a parsed path uses S3 versioning
// e.g. 'bucket/object.usd' returns False
//      'bucket/object.usd?versionId=abc123' returns True
const bool UsesVersioning(const std::string& path) {
  return path.find("versionId=") != std::string::npos;
}

// Get the version ID of a parsed path uses S3 versioning
// e.g. 'bucket/object.usd' returns an empty string
//      'bucket/object.usd?versionId=abc123' returns abc123
const std::string GetObjectVersionid(const std::string& path) {
  const uint i = path.find_first_of("versionId=");
  return (i != std::string::npos) ? path.substr(i + 10) : std::string();
}

// get an environment variable
std::string GetEnvVar(const std::string& env_var,
                      const std::string& default_value) {
  const auto env_var_value = getenv(env_var.c_str());
  return (env_var_value != nullptr) ? env_var_value : default_value;
}

}  // namespace

namespace usd_s3 {
Aws::SDKOptions options;
Aws::S3::S3Client* s3_client;

enum CacheState { CACHE_MISSING, CACHE_NEEDS_FETCHING, CACHE_FETCHED };

struct Cache {
  CacheState state;
  std::string local_path;
  double timestamp;  // date last modified
  bool is_pinned;    // pinned (versioned) objects don't need to be checked for
                     // changes
  std::string ETag;  // md5 hash
};

std::map<std::string, Cache> cached_requests;

// Determine a local path for an asset
std::string GeneratePath(const std::string& path) {
  const std::string local_dir = GetEnvVar(CACHE_PATH_ENV_VAR, "/tmp");
  return TfNormPath(local_dir + "/" + GetBucketName(path) + "/" +
                    GetObjectName(path));
}

// Check / resolve an asset with an S3 HEAD request and store the result in the
// cache Set CACHE_NEEDS_FETCHING if the asset was updated Requires the asset to
// be fetched before --
std::string check_object(const std::string& path, Cache& cache) {
  if (s3_client == nullptr) {
    return std::string();
  }

  Aws::S3::Model::HeadObjectRequest head_request;
  Aws::String bucket_name = GetBucketName(path).c_str();
  Aws::String object_name = GetObjectName(path).c_str();
  head_request.WithBucket(bucket_name).WithKey(object_name);

  if (UsesVersioning(path)) {
    Aws::String object_versionid = GetObjectVersionid(path).c_str();
    head_request.WithVersionId(object_versionid);
    cache.is_pinned = true;
  }

  auto head_object_outcome = s3_client->HeadObject(head_request);

  if (head_object_outcome.IsSuccess()) {
    // TODO set local_dir in S3 constructor
    double date_modified = head_object_outcome.GetResult()
                               .GetLastModified()
                               .SecondsWithMSPrecision();
    // check
    std::string local_path = GeneratePath(path);
    if (date_modified > cache.timestamp) {
      cache.state = CACHE_NEEDS_FETCHING;
    }
    cache.timestamp = date_modified;
    cache.local_path = local_path;

    return local_path;
  } else {
    cache.timestamp = INVALID_TIME;
    std::cout << "HeadObjects error: "
              << head_object_outcome.GetError().GetExceptionName() << " "
              << head_object_outcome.GetError().GetMessage() << std::endl;
    return std::string();
  };
}

// Fetch an asset from S3 to the local_path set in the cache object.
// Check for the presence of a local cache and only fetch the asset
// when it was modified after the cached timestamp.
bool fetch_object(const std::string& path, Cache& cache) {
  if (s3_client == nullptr) {
    return false;
  }

  Aws::S3::Model::GetObjectRequest object_request;
  Aws::String bucket_name = GetBucketName(path).c_str();
  Aws::String object_name = GetObjectName(path).c_str();
  object_request.WithBucket(bucket_name).WithKey(object_name);

  if (UsesVersioning(path)) {
    Aws::String object_versionid = GetObjectVersionid(path).c_str();
    object_request.WithVersionId(object_versionid);
    cache.is_pinned = true;
  }

  double local_date_modified_old = cache.timestamp;

  // Only download the asset if there's no local copy or if the local copy is
  // outdated The GET request returns a 304 (not modified).
  const std::string local_path = cache.local_path;
  if (TfPathExists(local_path)) {
    double local_date_modified;
    if (ArchGetModificationTime(local_path.c_str(), &local_date_modified)) {
      cache.timestamp = local_date_modified;
      object_request.WithIfModifiedSince(local_date_modified);
    }
  }

  auto get_object_outcome = s3_client->GetObject(object_request);

  if (get_object_outcome.IsSuccess()) {
    // prepare cache directory
    const std::string& bucket_path =
        cache.local_path.substr(0, cache.local_path.find_last_of('/'));
    if (!TfIsDir(bucket_path)) {
      bool isSuccess = TfMakeDirs(bucket_path);
      if (!isSuccess) {
        return false;
      }
    }
    cache.timestamp = local_date_modified_old;
    Aws::OFStream local_file;
    local_file.open(cache.local_path, std::ios::out | std::ios::binary);
    local_file << get_object_outcome.GetResult().GetBody().rdbuf();
    cache.timestamp = get_object_outcome.GetResult()
                          .GetLastModified()
                          .SecondsWithMSPrecision();
    // get_object_outcome.GetResult().GetVersionId().c_str());
    cache.state = CACHE_FETCHED;
    cache.ETag = get_object_outcome.GetResult().GetETag().c_str();
    return true;
  } else {
    if (get_object_outcome.GetError().GetResponseCode() ==
        Aws::Http::HttpResponseCode::NOT_MODIFIED) {
      // cache.timestamp =
      // get_object_outcome.GetResult().GetLastModified().SecondsWithMSPrecision();
      cache.state = CACHE_FETCHED;
      return true;
    }
    std::cout << "GetObject error: "
              << get_object_outcome.GetError().GetExceptionName() << " "
              << get_object_outcome.GetError().GetMessage() << std::endl;
    return false;
  }
}

S3::S3() {
  Aws::InitAPI(options);

  Aws::Client::ClientConfiguration config;
  config.scheme = Aws::Http::Scheme::HTTP;

  // set a custom endpoint e.g. an ActiveScale system node or minio server
  if (!GetEnvVar(ENDPOINT_ENV_VAR, "").empty()) {
    config.endpointOverride = (GetEnvVar(ENDPOINT_ENV_VAR, "")).c_str();
  }
  if (!GetEnvVar(PROXY_HOST_ENV_VAR, "").empty()) {
    config.proxyHost = GetEnvVar(PROXY_HOST_ENV_VAR, "").c_str();
    config.proxyPort = atoi(GetEnvVar(PROXY_PORT_ENV_VAR, "80").c_str());
  }

  config.connectTimeoutMs = 3000;
  config.requestTimeoutMs = 3000;

  // create a client with useVirtualAddressing=false to use path style
  // addressing see https://github.com/aws/aws-sdk-cpp/issues/587
  s3_client = Aws::New<Aws::S3::S3Client>(
      "s3resolver", config,
      Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);
}

S3::~S3() {
  Aws::Delete(s3_client);
  Aws::ShutdownAPI(options);
}

// Resolve an asset path such as 's3://hello/world.usd'
// Checks if the asset exists and returns a local path for the asset
std::string S3::ResolveName(const std::string& asset_path) {
  const auto path = ParsePath(asset_path);
  const auto cached_result = cached_requests.find(path);
  if (cached_result != cached_requests.end()) {
    if (cached_result->second.state == CACHE_FETCHED) {
      return check_object(path, cached_result->second);
    }
    if (cached_result->second.state != CACHE_MISSING) {
      return cached_result->second.local_path;
    }
    return "s3://" + cached_result->second.local_path;
  } else {
    Cache cache{CACHE_NEEDS_FETCHING, GeneratePath(path)};
    // std::string result = check_object(path, cache);
    cached_requests.insert(std::make_pair("s3://" + path, cache));
    return cache.local_path;
  }
}

// Update asset info for resolved assets
// If the asset needs fetching, nothing is done as the cache is updated during
// the fetch phase If the asset doesn't need fetching, also do nothing (lol)
void S3::UpdateAssetInfo(const std::string& asset_path) {
  // const auto path = parse_path(asset_path);
  // const auto cached_result = cached_requests.find(path);
  // if (cached_result != cached_requests.end()) {
  //     //
  //     if (cached_result->second.state == CACHE_NEEDS_FETCHING) {
  //         return;
  //     }

  //     TF_DEBUG(S3_DBG).Msg("S3: update_asset_info %s cache state %d\n",
  //     path.c_str(), cached_result->second.state);
  //     //cached_result->second.state = CACHE_NEEDS_FETCHING;
  //     //cached_result->second.timestamp = INVALID_TIME;
  //     //check_object(path, cached_result->second);
  // }
}

// Fetch an asset to a local path
// The asset should be resolved first and exist in the cache
bool S3::FetchAsset(const std::string& asset_path,
                    const std::string& local_path) {
  const auto path = ParsePath(asset_path);
  if (s3_client == nullptr) {
    return false;
  }

  const auto cached_result = cached_requests.find(path);
  if (cached_result == cached_requests.end()) {
    S3_WARN("[S3Resolver] %s was not resolved before fetching!", path.c_str());
    return false;
  }

  if (cached_result->second.state == CACHE_NEEDS_FETCHING) {
    cached_result->second.state =
        CACHE_MISSING;  // we'll set this up if fetching is successful
    bool success = fetch_object(path, cached_result->second);
    return success;
  }
  return true;
}

// returns true if the path matches the S3 schema
bool S3::MatchesSchema(const std::string& path) {
  constexpr auto schema_length_short = cexpr_strlen(usd_s3::S3_PREFIX_SHORT);
  return path.compare(0, schema_length_short, usd_s3::S3_PREFIX_SHORT) == 0;
}

// returns the timestamp of the local cached asset
double S3::GetTimestamp(const std::string& asset_path) {
  const auto path = ParsePath(asset_path);
  if (s3_client == nullptr) {
    return 1.0;
  }

  const auto cached_result = cached_requests.find(path);
  if (cached_result == cached_requests.end() ||
      cached_result->second.state == CACHE_MISSING) {
    S3_WARN("[S3Resolver] %s is missing when querying timestamps!",
            path.c_str());
    return 1.0;
  } else {
    return cached_result->second.timestamp;
  }
}

// refresh all assets with this prefix
void S3::Refresh(const std::string& prefix) {
  if (prefix.empty()) {
    // refresh all assets
    cached_requests.clear();
  } else {
    // and reload based on S3 list operation with prefix
    cached_requests.clear();
  }
}

}  // namespace usd_s3
