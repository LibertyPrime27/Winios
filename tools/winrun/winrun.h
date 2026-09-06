/* winrun as a library: run a Windows executable on xcore in-process.
 *
 * The command-line tool is a one-line main() over this. The device build
 * (tools/memprobe) calls it directly, which is why it resets every global the
 * runtime owns on entry -- including the block cache, since a second
 * executable maps its own code at the same guest addresses.
 *
 * Not reentrant and not thread-safe: one guest process at a time.
 */
#ifndef WINRUN_H
#define WINRUN_H

#ifdef __cplusplus
extern "C" {
#endif

/* argv[0..] as the tool takes them: [-v] program.exe [args...].
 * Returns the guest's exit code, or 2 on a setup failure. */
int winrun_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
