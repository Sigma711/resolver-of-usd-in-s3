#include "resolver.h"

#include <pxr/base/tf/fileUtils.h>
#include <pxr/base/tf/pathUtils.h>
#include <pxr/base/tf/registryManager.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/assetInfo.h>
#include <pxr/usd/ar/defaultResolver.h>
#include <pxr/usd/ar/definePackageResolver.h>
#include <pxr/usd/ar/defineResolver.h>
#include <pxr/usd/ar/packageResolver.h>
#include <pxr/usd/ar/resolverContext.h>
#include <pxr/usd/ar/threadLocalScopedCache.h>
#include <pxr/usd/usd/zipFile.h>
#include <tbb/concurrent_hash_map.h>

#include <memory>

#include "s3.h"

PXR_NAMESPACE_OPEN_SCOPE

#define S3_WARN TF_WARN

namespace {
usd_s3::S3 g_s3;
}

AR_DEFINE_RESOLVER(S3Resolver, ArResolver)

struct S3Resolver::Cache {
  using PathToResolvedPathMap =
      tbb::concurrent_hash_map<std::string, std::string>;
  PathToResolvedPathMap path_to_resolved_path_map_;
};

S3Resolver::S3Resolver() : ArDefaultResolver() {}

std::string S3Resolver::Resolve(const std::string& path) {
  return S3Resolver::ResolveWithAssetInfo(path, nullptr);
}

std::string S3Resolver::ResolveWithAssetInfo(const std::string& path,
                                             ArAssetInfo* asset_info) {
  if (path.empty()) {
    return path;
  }
  if (g_s3.matches_schema(path)) {
    return g_s3.resolve_name(path);
  }
  if (CachePtr current_cache = GetCurrentCache()) {
    Cache::PathToResolvedPathMap::accessor accessor;
    if (current_cache->path_to_resolved_path_map_.insert(
            accessor, std::make_pair(path, std::string()))) {
      accessor->second =
          ArDefaultResolver::ResolveWithAssetInfo(path, asset_info);
    }
    return accessor->second;
  }
  return ArDefaultResolver::ResolveWithAssetInfo(path, asset_info);
}

bool S3Resolver::IsRelativePath(const std::string& path) {
  return !g_s3.matches_schema(path) && ArDefaultResolver::IsRelativePath(path);
}

VtValue S3Resolver::GetModificationTimestamp(const std::string& path,
                                             const std::string& resolved_path) {
  if (g_s3.matches_schema(path)) {
    return VtValue(g_s3.get_timestamp(path));
  }
  return ArDefaultResolver::GetModificationTimestamp(path, resolved_path);
}

void S3Resolver::UpdateAssetInfo(const std::string& identifier,
                                 const std::string& file_path,
                                 const std::string& file_version,
                                 ArAssetInfo* asset_info) {
  if (g_s3.matches_schema(identifier)) {
    g_s3.update_asset_info(identifier);
  }
  ArDefaultResolver::UpdateAssetInfo(identifier, file_path, file_version,
                                     asset_info);
}

bool S3Resolver::FetchToLocalResolvedPath(const std::string& path,
                                          const std::string& resolved_path) {
  if (g_s3.matches_schema(path)) {
    return g_s3.fetch_asset(path, resolved_path);
  } else {
    return ArDefaultResolver::FetchToLocalResolvedPath(path, resolved_path);
  }
}

void S3Resolver::ConfigureResolverForAsset(const std::string& path) {
  ArDefaultResolver::ConfigureResolverForAsset(path);
}

void S3Resolver::RefreshContext(const ArResolverContext& context) {
  g_s3.refresh("");
  // This is empty anyway
  ArDefaultResolver::RefreshContext(context);
}

void S3Resolver::BeginCacheScope(VtValue* cache_scope_data) {
  cache_.BeginCacheScope(cache_scope_data);
}

void S3Resolver::EndCacheScope(VtValue* cache_scope_data) {
  cache_.EndCacheScope(cache_scope_data);
}

S3Resolver::CachePtr S3Resolver::GetCurrentCache() {
  return cache_.GetCurrentCache();
}

PXR_NAMESPACE_CLOSE_SCOPE
