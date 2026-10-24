#include "KeyImportPermit.h"
#include <semaphore>
#include <algorithm>

namespace nordssh::ssh {
namespace {
std::counting_semaphore<2> permits{2};
}
KeyImportPermit::KeyImportPermit(const OperationControl& control) {
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (control.stop.stop_requested() || now >= control.deadline || control.deadline - now > std::chrono::seconds(30))
            throw SessionError();
        if (permits.try_acquire_for((std::min)(control.deadline - now,
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(25))))) {
            // Cancellation may arrive during acquisition, including on an idle
            // semaphore. Never enter a costly decode after that cancellation.
            if (control.stop.stop_requested() || std::chrono::steady_clock::now() >= control.deadline) {
                permits.release();
                throw SessionError();
            }
            return;
        }
    }
}
KeyImportPermit::~KeyImportPermit() { permits.release(); }
} // namespace nordssh::ssh
