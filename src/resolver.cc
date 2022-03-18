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

namespace {
usd_s3::S3 g_s3;
}

struct S3Resolver::Cache {
  using _PathToResolvedPathMap =
      tbb::concurrent_hash_map<std::string, std::string>;
  _PathToResolvedPathMap _pathToResolvedPathMap;
};

PXR_NAMESPACE_CLOSE_SCOPE
