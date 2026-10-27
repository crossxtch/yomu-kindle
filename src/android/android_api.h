#pragma once

/* zig does not set the bionic API level. The NDK clang triple does.
 * This header is force-included only on the zig path.
 */
#ifndef __ANDROID__
#define __ANDROID__ 1
#endif
#ifndef __ANDROID_API__
#define __ANDROID_API__ 22
#endif
