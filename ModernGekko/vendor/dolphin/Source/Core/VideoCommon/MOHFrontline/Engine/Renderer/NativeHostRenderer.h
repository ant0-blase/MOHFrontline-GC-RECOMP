#pragma once

namespace MOHFrontline::NativeHostRenderer
{
// Installs the first concrete NativeRender submitter.  Resources are created
// lazily on the GPU thread when the first matched PS3 MSH/DMF is submitted.
void Initialize();
void Shutdown();
}  // namespace MOHFrontline::NativeHostRenderer
