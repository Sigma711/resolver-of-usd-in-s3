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
};

#endif  // !SRC_S3_RESOLVER_H_
