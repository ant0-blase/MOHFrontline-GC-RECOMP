#pragma once
#include <algorithm>
#include <cstddef>

namespace MOHFrontline::NativeRender
{
// Limits the upload size, never the number of triangles submitted. The caller
// validates the packet before issuing the first batch so fallback stays atomic.
template <typename Submit>
void ForEachTriangleBatch(std::size_t count, std::size_t limit, Submit&& submit)
{
  if (limit == 0)
    return;
  for (std::size_t first = 0; first < count;)
  {
    const auto size = std::min(limit, count - first);
    submit(first, size);
    first += size;
  }
}
}
