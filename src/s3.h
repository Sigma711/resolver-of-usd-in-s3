#ifndef S3_H_
#define S3_H_

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace usd_s3 {

constexpr const char S3_PREFIX[] = "s3://";
constexpr const char S3_PREFIX_SINGLE[] = "s3:/";
constexpr const char S3_PREFIX_SHORT[] = "s3:";
constexpr const char S3_SUFFIX[] = ".s3";
constexpr const char CACHE_PATH_ENV_VAR[] = "USD_S3_CACHE_PATH";
constexpr const char PROXY_HOST_ENV_VAR[] = "USD_S3_PROXY_HOST";
constexpr const char PROXY_PORT_ENV_VAR[] = "USD_S3_PROXY_PORT";
constexpr const char ENDPOINT_ENV_VAR[] = "USD_S3_ENDPOINT";

class S3 {
 public:
  S3();
  ~S3();

  bool MatchesSchema(const std::string& path);

  std::string ResolveName(const std::string& path);
  void UpdateAssetInfo(const std::string& asset_path);
  bool FetchAsset(const std::string& asset_path, const std::string& local_path);
  double GetTimestamp(const std::string& asset_path);

  void Refresh(const std::string& prefix);

 private:
};

}  // namespace usd_s3

#endif  // !S3_H_
