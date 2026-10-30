#include "vc/CoreMlMaskProvider.h"

namespace vc {

std::unique_ptr<AsyncMaskProvider> makeCoreMlMaskProvider(const std::string&, int, std::string& error)
{
    error = "CoreML / Neural Engine is only available on macOS";
    return nullptr;
}

} // namespace vc
