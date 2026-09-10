import Foundation

/// Whether the dynarec can run, and getting it running without being asked.
///
/// This exists because the indicator was lying. `xc_jit_available()` returns
/// `g_external` on iOS — 0 until somebody hands xcore a region of executable
/// memory — and the only thing that ever did that was a button inside the old
/// diagnostics screen. So a device where the JIT was perfectly usable still
/// showed "disabled", which is the worst kind of wrong: it tells you to go
/// and fix something that is not broken.
///
/// The usable case it was missing is the normal one. People launch through
/// StikDebug with LiveContainer, and JIT is already enabled by the time the
/// app's own code runs — the process has CS_DEBUGGED and the RW/RX alias pair
/// maps. Nothing needs to be attached; the arena just needs to be created and
/// handed over. So that is attempted at launch.
///
/// What makes attempting it safe is the breadcrumb. `jit_probe_safe` never
/// jumps — it reads the code-signing flags and tries the mapping and the
/// protection change, all recoverable. Only if that looks right is the
/// executing probe run, and that one writes a marker before each step, so an
/// attempt that kills the process is *known* on the next launch and is not
/// repeated. Without that this would be a crash loop, which is why it is not
/// simply a matter of trying and seeing.
enum JIT {

    private static var markerPath: String {
        (ResultStore.containerDir?.appendingPathComponent("jit.marker").path) ?? ""
    }

    /// Set once `ensure()` has handed an arena to xcore, so a second call does
    /// not repeat the work — and, more importantly, does not run the
    /// executing probe twice.
    private static var installed = false
    private static var tried = false

    /// True when xcore will actually compile. Not a capability flag: it is
    /// the truth about this launch.
    static var isReady: Bool { xc_jit_available() != 0 }

    /// What to tell the person, given how far it got.
    static var explanation: String {
        if isReady {
            return "Guest code is compiled to ARM64 — roughly a hundred times the "
                 + "interpreter."
        }
        var r = jit_result()
        markerPath.withCString { jit_probe_safe(&r, $0) }
        // if/else rather than a switch: a C enum arrives in Swift as a struct
        // of static members, and this file cannot be compiled where it was
        // written -- so the construction with the fewest ways to be wrong wins.
        if r.state == JIT_CRASHED {
            return "An earlier attempt to use executable memory did not come back, "
                 + "so it has not been retried. Reset results on the Enable JIT "
                 + "screen to try again."
        }
        if r.state == JIT_ABSENT {
            return "This process has no permission to create executable memory. "
                 + "Launch through StikDebug (with LiveContainer), or use Enable "
                 + "JIT to attach one now. Until then everything is interpreted, "
                 + "which is far slower."
        }
        if r.state == JIT_FLAG_ONLY {
            return "The debugger flag is set but the executable mapping was "
                 + "refused. Enable JIT walks through it and reports where it "
                 + "stops."
        }
        return "Executable memory is available but has not been handed to the "
             + "emulator yet. Open Enable JIT."
    }

    /// Try to have a working JIT, without asking anyone. Call at launch.
    ///
    /// Returns true if xcore ended up with an arena. Safe to call more than
    /// once; the work happens at most once per launch.
    @discardableResult
    static func ensure() -> Bool {
        if installed || isReady { installed = true; return true }
        if tried { return false }
        tried = true

        // Look first, and never at the cost of the launch: this call reads the
        // code-signing status and tries the mapping, and does not execute.
        var safe = jit_result()
        markerPath.withCString { jit_probe_safe(&safe, $0) }

        // A previous attempt that did not return leaves a breadcrumb. Retrying
        // it automatically is how an app becomes impossible to open.
        if safe.state == JIT_CRASHED { return false }
        // No permission, or the mapping was refused: there is nothing here to
        // hand over, and finding that out cost nothing.
        if safe.state == JIT_ABSENT || safe.state == JIT_FLAG_ONLY { return false }

        // The mapping works, so ask for the shared arena and give it to xcore.
        // `jit_arena_shared` is the same one the JIT screen would use, so a
        // later visit there finds it already made rather than making a second.
        var r = jit_result()
        var fresh: Int32 = 0
        let kb = nextArenaKB()
        guard let arena = markerPath.withCString({ m in
            jit_arena_shared(size_t(kb * 1024), &r, m, &fresh)
        }) else { return false }

        installed = xc_jit_set_code(arena.pointee.rw, arena.pointee.rx,
                                    arena.pointee.size) == 1
        if installed { arenaWorked(kb) }
        return installed
    }

    /// The ladder of arena sizes, and how far up it this launch should reach.
    ///
    /// One megabyte is not enough to be worth having. The dynarec's code
    /// arena is a bump allocator: when it fills, everything compiled so far is
    /// thrown away and compiled again. A GameMaker game measured on an iPad
    /// filled a 1 MB arena 371 times in seventy seconds and spent most of its
    /// time recompiling, at under three frames a second. The working set is
    /// tens of megabytes, so the arena has to be too.
    ///
    /// Climbing is safe because of the breadcrumb, which is the same protocol
    /// the JIT screen uses. The size about to be tried is written down first;
    /// a launch that finds one still written knows that attempt never came
    /// back, records the size as the ceiling, and drops to the largest that
    /// has worked. So the worst a too-large arena costs is one restart, and it
    /// is never tried twice.
    private static let arenaLadder = [1024, 4096, 16384, 65536, 262144]   // 1 MB … 256 MB

    private static func nextArenaKB() -> Int {
        let d = UserDefaults.standard
        var good = d.object(forKey: "arenaGoodKB") as? Int ?? 0
        var bad = d.object(forKey: "arenaBadKB") as? Int ?? 0
        // A size still written down is one whose attempt never finished.
        let pending = d.object(forKey: "arenaPendingKB") as? Int ?? 0
        if pending != 0 {
            if bad == 0 || pending < bad { bad = pending; d.set(bad, forKey: "arenaBadKB") }
            d.set(0, forKey: "arenaPendingKB")
        }
        if good == 0 { good = d.object(forKey: "arenaKB") as? Int ?? 0 }
        // One rung above the largest that has worked, while the ceiling is
        // still unknown; otherwise stay where it is known to work.
        var want = good > 0 ? good : arenaLadder[0]
        if bad == 0, let next = arenaLadder.first(where: { $0 > want }) { want = next }
        if bad != 0, want >= bad { want = good > 0 ? good : arenaLadder[0] }
        d.set(want, forKey: "arenaPendingKB")
        d.synchronize()          // it has to survive a launch that does not return
        return want
    }

    private static func arenaWorked(_ kb: Int) {
        let d = UserDefaults.standard
        if (d.object(forKey: "arenaGoodKB") as? Int ?? 0) < kb { d.set(kb, forKey: "arenaGoodKB") }
        d.set(0, forKey: "arenaPendingKB")
        d.synchronize()
    }
}
