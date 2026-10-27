#pragma once

#include <cstddef>
#include <cstdint>

/* The Xteink typesetter borrows a reserved heap hole. The Paperwhite does not. */

namespace ScratchHeap {

inline bool reserve(uint16_t, uint16_t) { return true; }
inline void release() {}
inline uint8_t *data() { return nullptr; }
inline size_t size() { return 0; }

}  // namespace ScratchHeap
