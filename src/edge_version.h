#ifndef SRC_EDGE_VERSION_H_
#define SRC_EDGE_VERSION_H_

// x-release-please-start-major
#define EDGE_MAJOR_VERSION 0
// x-release-please-end
// x-release-please-start-minor
#define EDGE_MINOR_VERSION 2
// x-release-please-end
// x-release-please-start-patch
#define EDGE_PATCH_VERSION 5
// x-release-please-end

#ifndef EDGE_STRINGIFY
#define EDGE_STRINGIFY(n) EDGE_STRINGIFY_HELPER(n)
#define EDGE_STRINGIFY_HELPER(n) #n
#endif

#ifndef EDGE_VERSION_COMMIT
#define EDGE_VERSION_COMMIT "unknown"
#endif

#ifndef EDGE_VERSION_SUFFIX
#define EDGE_VERSION_SUFFIX ""
#endif

#define EDGE_VERSION_BASE_STRING EDGE_STRINGIFY(EDGE_MAJOR_VERSION) "." \
                                EDGE_STRINGIFY(EDGE_MINOR_VERSION) "." \
                                EDGE_STRINGIFY(EDGE_PATCH_VERSION)
#define EDGE_VERSION_STRING EDGE_VERSION_BASE_STRING EDGE_VERSION_SUFFIX
#define EDGE_VERSION "v" EDGE_VERSION_STRING

#endif  // SRC_EDGE_VERSION_H_
