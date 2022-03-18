#ifndef SRC_S3_RESOLVER_H_
#define SRC_S3_RESOLVER_H_

#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/defaultResolver.h>

#include <string>

#include "pxr/usd/ar/threadLocalScopedCache.h"

PXR_NAMESPACE_OPEN_SCOPE

class S3Resolver : public ArDefaultResolver {
 public:
  S3Resolver();
  ~S3Resolver();

  std::string Resolve(const std::string& path);

  std::string ResolveWithAssetInfo(const std::string& path,
                                   ArAssetInfo* assetInfo);

  bool IsRelativePath(const std::string& path);

  VtValue GetModificationTimestamp(const std::string& path,
                                   const std::string& resolvedPath);

  void UpdateAssetInfo(const std::string& identifier,
                       const std::string& filePath,
                       const std::string& fileVersion, ArAssetInfo* assetInfo);

  bool FetchToLocalResolvedPath(const std::string& path,
                                const std::string& resolvedPath);

  void ConfigureResolverForAsset(const std::string& path);

  void RefreshContext(const ArResolverContext& context);

  void BeginCacheScope(VtValue* cacheScopeData);

  void EndCacheScope(VtValue* cacheScopeData);

 private:
  struct Cache;
  using ResolveCache = ArThreadLocalScopedCache<Cache>;
  using CachePtr = ResolveCache::CachePtr;

  CachePtr GetCurrentCache();

  ResolveCache cache_;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // !SRC_S3_RESOLVER_H_
