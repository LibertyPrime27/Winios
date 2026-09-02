import Foundation
import os

/// One rung of the memory ladder, appended to the shared log after every step.
struct Rung: Codable {
    let step: Int
    let residentMB: Int          // cumulative touched bytes, in MB
    let availableMB: Int         // os_proc_available_memory() after this step
    let host: String             // "app" or "extension"
    let at: Date
}

/// Climbs memory in fixed steps, recording each rung durably before taking the
/// next one.
///
/// The recording order is the whole design. A process that hits the jetsam
/// limit is killed without warning -- there is no callback, no exception, no
/// chance to summarise at the end. So each rung is written and flushed BEFORE
/// the next allocation is attempted, and the answer is read from the last rung
/// that survived rather than from a return value that never arrives.
enum Ladder {
    static let log = Logger(subsystem: "winios.memprobe", category: "ladder")

    /// - Parameters:
    ///   - stepMB: bytes added per rung. 64 MB is a reasonable balance between
    ///     resolution and the number of writes.
    ///   - ceilingMB: stop voluntarily here. Set above any plausible limit when
    ///     you want the kill itself to be the answer.
    static func climb(host: String, stepMB: Int = 64, ceilingMB: Int = 16384) {
        let page = Int(getpagesize())
        let stepBytes = stepMB << 20
        var blocks: [UnsafeMutableRawPointer] = []
        var step = 0

        log.notice("ladder start host=\(host, privacy: .public) step=\(stepMB)MB page=\(page)")

        while (step + 1) * stepMB <= ceilingMB {
            guard let block = mp_alloc_touch(stepBytes, page) else {
                log.error("malloc refused at \(( step + 1) * stepMB)MB -- address space, not jetsam")
                break
            }
            // Consume the writes so they cannot be optimised away. Without a
            // reader the compiler may drop the stores, leaving pages clean and
            // the measurement meaningless.
            guard mp_verify(block, stepBytes, page) == 1 else {
                log.fault("pattern check failed -- probe is broken, not the limit")
                break
            }
            blocks.append(block)
            step += 1

            let rung = Rung(step: step,
                            residentMB: step * stepMB,
                            availableMB: Int(os_proc_available_memory()) >> 20,
                            host: host,
                            at: Date())
            ResultStore.append(rung)
            log.notice("rung \(step) resident=\(rung.residentMB)MB available=\(rung.availableMB)MB")

            // Give the OS a moment to account the pages. Without this the
            // ladder can outrun jetsam and die a rung or two past the real
            // limit, overstating the result.
            Thread.sleep(forTimeInterval: 0.15)
        }

        log.notice("ladder stopped voluntarily at \(step * stepMB)MB -- ceiling reached, not a limit")
        blocks.forEach { free($0) }
    }
}
