# The Win32 layer: Windows executables on xcore

**Where:** `win32/` (`pe.c` loader, `kernel32.c`, `msvcrt.c`, `com.c`, `d3d9.c`, `raster.c`, `w32.h`),
`tools/winrun/winrun.c` (runtime and command-line driver),
`tests/win32/` (mingw-built .exe guests with recorded output).

    winrun [-v] [-L dlldir] program.exe [args...]

This is the step from "a CPU that runs x86 code" to "a machine that runs
Windows programs": load a PE image, give it a process to live in, and answer
every DLL call it makes. It is the same idea Wine proved — the program's own
code runs unchanged; the operating system underneath it is reimplemented —
built here on our CPU instead of the host's.

## How an import becomes a host call

Every import the executable names resolves to a 16-byte **stub** in a guest
page whose first byte is `int3`. Calling the import stops the CPU with RIP one
past the `int3`; the runtime finds the stub, reads the arguments where the
calling convention left them (rcx/rdx/r8/r9 and the stack on x64; the stack on
x86), runs the host implementation, writes the result to rax (edx:eax for
64-bit results on x86, xmm0 / ST(0) for doubles), and returns to the caller —
popping the arguments too for stdcall. No guest code is generated for any of
this and the JIT is unaware of it: an `int3` is a breakpoint to the CPU, so
the same mechanism works through the interpreter and through compiled blocks.

Imports of *data* (msvcrt's `_iob`, `__initenv`, `_fmode`...) resolve to guest
memory the runtime owns; imports nobody implements resolve to a stub that
names itself when — if — it is called, so a missing function is a one-line
message rather than a crash.

Guest callbacks go the other way through `w32_call_guest()`: push a
return-to-host stub, set up the frame, re-enter the run loop until that stub
is hit. TLS callbacks, `_initterm`, `atexit` handlers and `qsort` comparators
all run this way, nested to any depth.

## DLLs the guest brings with it

An import names a DLL, and two different things can satisfy it.

**A DLL we implement on the host** — kernel32, msvcrt, ntdll, user32 — resolves
to stubs, as above. The host list always wins: a program shipping its own
`kernel32.dll` gets ours, which is the whole point of the exercise.

**A real PE DLL sitting next to the executable** is loaded the same way the
executable is — mapped, relocated, its own imports resolved, its TLS block
allocated — and the import resolves through its **export directory**. Exports
resolve by name (the name table is sorted, so it is a binary search) or by
ordinal, and an export whose address lands inside the export directory is a
**forwarder**: the bytes there are `"otherdll.SomeFunc"`, which is resolved
again from the start. `LoadLibrary`, `GetProcAddress` (by name and by
ordinal), `GetModuleHandle`, `GetModuleFileName` and `FreeLibrary` all work
against the same module table. `-L dir` adds a directory to search; otherwise
it is the executable's own directory and the working directory.

Two ordering rules matter, and both are observable in the test:

- **Loading is recursive and cycle-safe.** A module is registered in the table
  *before* its imports are resolved, so a dependency loop finds the half-built
  module instead of looping forever.
- **DllMain runs in dependency order** — which is *not* table order. Because a
  module is registered before its dependencies are, the table has dependents
  first; each module also records the order in which it *finished* loading,
  and that is what the attach walk sorts by. So `sub.dll` is attached before
  `mid.dll`, and `mid.dll`'s DllMain can call into `sub.dll`. Getting this
  backwards is the first bug this code had, and the test now names the order
  explicitly rather than checking a return value.

A DLL loaded later by `LoadLibrary` is attached before that call returns, so a
plugin host, a mod loader, or a late-bound `d3d9` sees an initialised module.
Nothing is ever unmapped: `FreeLibrary` decrements a count and returns
success, because an emulator that keeps a dead image mapped is strictly safer
than one that unmaps it under a live return address.

## COM, and d3d9.dll

Direct3D is not a set of exported functions. `dev->lpVtbl->Clear(dev, ...)`
compiles to an indirect call through a slot the guest computes itself, so an
interface is only usable if the **vtable lives in guest memory** and every
slot in it is something the guest can call — which is what the import stubs
already are. `com.c` builds a vtable per interface: one `int3` stub per slot,
landing in the same dispatcher an imported function does, with `this` as
argument 0 (pushed first for x86 stdcall, `rcx` on x64 — the same place in
both). An object's state lives in guest memory right after its vtable pointer
and reference count, so the host side holds nothing and there is nothing to
free or reset.

`d3d9.c` is that mechanism plus one export. `Direct3DCreate9` returns an
`IDirect3D9`; `CreateDevice` returns an `IDirect3DDevice9` with a back buffer
allocated in **guest memory**, which is what makes both ends work — the guest
can lock and read it directly, and the host can hand the same bytes to Metal
without a copy through a second address space. `Clear`, `BeginScene`,
`EndScene`, `Present`, `GetBackBuffer` and the surface's `LockRect` are real;
the state setters accept and ignore, because there is nothing to draw with
yet. Where a presented frame goes is a host callback (`w32_set_present`):
nothing by default, a `.ppm` per frame under `WINRUN_PRESENT_PPM=<prefix>`,
and on the device a Metal texture MemProbe draws full screen.

Stopping a guest that is drawing frames goes the other way, and deliberately
does not reach into it: `w32_d3d9_device_lost(1)` makes `Present` and
`TestCooperativeLevel` return `D3DERR_DEVICELOST`, which is what a real driver
returns when the display mode changes or the machine sleeps. A program that
checks its `Present` result — every game does — leaves its own loop and exits
normally. `test_winrun_lib` runs `d3dloop32.exe` with no frame limit, trips
the flag after five frames, and requires that it stopped there and exited 0.

**Vtable order is load-bearing.** A slot in the wrong place is an indirect
call to the wrong function, so the tables carry explicit slot numbers taken
from `d3d9.h`, and every slot that is not implemented gets a stub that names
its interface and index. That is not decoration: the first version of
`IDirect3DDevice9` was missing `CreateDepthStencilSurface` at slot 29, so
everything after it was one out, and what came back was
`call to unimplemented IDirect3DDevice9::slot 57` — the guest asking for
`SetRenderState` at 57 while the table had it at 56.

### Drawing

`CreateVertexBuffer` gives the guest a buffer in **guest memory** that it
locks and fills itself, `SetStreamSource` and `SetFVF` bind it, and
`DrawPrimitive` / `DrawPrimitiveUP` assemble triangles from it — lists, strips
and fans. One vertex format is accepted: `D3DFVF_XYZRHW | D3DFVF_DIFFUSE`,
a position already in screen space plus a colour. That is deliberate: it
isolates the parts that had never run before (vertex fetch, primitive
assembly, rasterization, writing the render target) from the parts that do not
exist yet (the world/view/projection matrices, lighting, texture stages).
Anything else is refused with a message rather than drawn wrong.

The pixels come from `raster.c`, a **reference rasterizer** — not the renderer.
Its job is to make a draw call checkable without a GPU: the same guest that
draws a triangle on the iPad draws it on a Linux runner and under qemu, and
the frame checksums have to match. Every other layer here is verified that way
and a GPU-only draw path would have been the first with no coverage at all.
Determinism is therefore designed in rather than hoped for: vertex positions
become 28.4 fixed point by multiplying by 16 (exact for a float, being a power
of two) and everything after that is integer edge functions and integer
barycentric colour, so there is no floating-point rounding to differ between
an x86 runner, qemu on aarch64 and an M3, and nothing a compiler can contract
into an fma. `d3ddraw.exe` produces checksum `acde04d16c79039c` in all of
them, and identically as PE32 and PE32+.

What is still missing is the GPU. The rasterizer runs on every core the
machine has (`w32_d3d11_triangles_shaded` deals a draw's rows out in bands of
eight, one worker per core, each walking the triangles in order, so a pixel is
always drawn by one thread in program order and the checksum of a frame is
the same on one core or eight), which is a multiple of the speed and not a
change of kind. Pointing the draw path at Metal, so the back buffer becomes a
render target instead of memory the CPU writes, is still the step after this
one; d12mt on the submodule compiles DXIL (Direct3D 12), so the DXBC of D3D11
and the tokens of D3D9 would need a translator in front of it. The rasterizer
stays as the reference the GPU path is checked against, and as the fallback
where no GPU path is available.

## d3d11.dll and dxgi.dll

Everything from 2009 onward asks for Direct3D 11, so `d3d11.c` is the same
mechanism again at the scale D3D11 actually has: twenty object types, twelve
vtables, and 115 methods on `ID3D11DeviceContext` alone. `D3D11CreateDevice`
and `D3D11CreateDeviceAndSwapChain` return a device, an immediate context and
a swap chain whose back buffer is the screen surface; `CreateDXGIFactory`
returns a factory, and the device's `QueryInterface` hands back `IDXGIDevice`
and `IDXGIDevice1` so the usual `device -> IDXGIDevice -> adapter -> factory`
walk a game does at startup completes.

**Every slot count is checked by the compiler.** With 115 methods in one
interface, a table one short is not something anyone finds by reading it — the
call goes to the neighbouring method and the program fails later, somewhere
else. So the counts were taken mechanically out of `d3d11.h` and `dxgi.h` by
parsing the `typedef struct IXxxVtbl` blocks, and each table carries a
`_Static_assert` on its length:

```c
_Static_assert(NM(device_methods)  == 43,  "ID3D11Device has 43 methods");
_Static_assert(NM(context_methods) == 115, "ID3D11DeviceContext has 115 methods");
```

That is not hypothetical tidiness. `CheckFeatureSupport` was missing from the
device table, which put `GetFeatureLevel` where `GetCreationFlags` should have
been; the assert is what makes the next such omission a build error instead of
a bug report. The same pass found `ID3D11Query` needing nine slots rather than
seven.

**Structure sizes are measured, not derived.** `GetDesc` calls write into a
struct on the caller's stack, and a struct ending in pointer-sized members is
a different size in each bitness. Clearing 320 bytes into a 292-byte
`DXGI_ADAPTER_DESC` smashed the caller's frame and faulted much later with
`rsp = 0xfffffffc` — after every check in the test had already passed. The
sizes now in the file (`292`/`304` for the adapter description, `60`/`72` for
the swap-chain description) came from compiling a mingw `sizeof`/`offsetof`
probe and running it under `winrun` in both bitnesses, which is the only way
to get them right rather than plausible.

### What the pipeline does, and what it does not

**The shaders run** (since September 2026; before that they were parsed and not
executed). `CreateVertexShader` and `CreatePixelShader` are handed DXBC
containers; `dxbc.c` parses the chunk directory, the `ISGN`/`OSGN` signatures
and keeps a copy of the `SHDR`/`SHEX` code, and `dxbc_exec.c` decodes the code
once and interprets it per vertex and per pixel: the whole model 4/5 operand
encoding, temporaries, indexable arrays, constant buffers and immediate
constants, `if`/`loop`/`break`/`ret`, float, integer and unsigned arithmetic,
sampling, texel loads and resource queries. `d3d11.c` joins the two stages by
semantic as the hardware does and tracks four slots each of constant buffers,
textures and samplers. What is not interpreted — geometry/hull/domain/compute,
calls, switches, comparison sampling, gather — is refused by name, the draw
falls back to the fixed-function reading of the signatures described next, and
the report says which shader and why.

The fixed-function reading remains the fallback: the vertex stage transforms
`POSITION` by the 4x4 matrix in constant buffer 0 and passes `TEXCOORD` and
`COLOR` through, and the pixel stage samples texture 0 and modulates by the
interpolated colour. It is also what a container with signatures and no code
gets — which is what the hand-built shaders in `d11test.c` are.

The interpreter's float arithmetic is compiled with contraction off (see
`CMakeLists.txt`), so `a*b+c` is two roundings on x86 and ARM alike and a frame
checksum recorded on one machine holds on the other. It is slow — every pixel
runs the pixel program — and the CPU rasterizer is the ceiling regardless; what
it buys is a *correct* frame from a shader-driven 2D engine such as GameMaker,
whose vertex transform is a matrix in a constant buffer that no reading of a
signature could see. `test_dxbc.c` assembles token streams by hand and checks
results to the bit.

Two more limits worth stating plainly: there is **no depth buffer** (a
depth-stencil view is accepted and ignored, so a scene that relies on z
ordering draws in submission order), and interpolation is **affine rather than
perspective-correct** (a large triangle seen edge-on has its texture visibly
skewed). Neither is hidden behind a "not supported" that would stop a game
starting; both change what you see.

The vertex path is real, though. `CreateInputLayout` keeps the element array,
including `D3D11_APPEND_ALIGNED_ELEMENT`, and `fetch_semantic` walks it to
find a semantic and index in a bound vertex buffer; `read_attr` decodes the
DXGI formats a game actually uses for vertex data, including 16-bit floats and
both byte orders of 8-bit colour. `Draw`, `DrawIndexed`, `DrawInstanced` and
`DrawIndexedInstanced` assemble lists and strips (with the odd-triangle winding
swap), the last vertex is bounds-checked against the buffer's size, and
`Map`/`Unmap` and `UpdateSubresource` write into buffers that live in guest
memory just as D3D9's do.

The pixels come from `d3d11_raster.c`, built the same way as `raster.c` and
for the same reason: 28.4 fixed point for coordinates, 16.16 for texture
coordinates, integer edge functions for the barycentric weights. `d11test.exe`
— which builds its DXBC containers by hand, creates the whole object graph and
draws an indexed textured quad and a Gouraud strip — presents a frame whose
checksum is `311139ad` at 320x200 on an x86 runner, under qemu on aarch64, and
in both bitnesses. Being the same number in all of those is the only reason a
checksum is worth recording at all.

## Text

Until recently the text was a **stroke font**: polylines on a design grid,
drawn with line segments. Its comment gave three good reasons — there is no
font file to load on an iPhone, a bitmap font locks the text to one size, and
a dialog asks for whatever size its template says — and the result still
looked like a pen plotter, which is the single most obvious way a screenshot
of this did not look like Windows.

`truetype.c` is a TrueType parser and rasterizer: six tables (`head`, `hhea`,
`maxp`, `cmap`, `loca`, `glyf`, `hmtx`), composite glyphs, and a scanline fill
with four sub-scanlines per pixel row and exact horizontal coverage. No
hinting — the bytecode interpreter is a large piece of work whose whole
purpose is to snap stems to a pixel grid, and at 11 to 16 pixels antialiased
unhinted text is closer to a modern Windows than badly hinted text is. What it
does instead is **stem darkening**: coverage straight off the rasterizer is
linearly correct and reads washed out next to hinted text, so the curve is
bent by `a + a(255-a)/512` — a gamma of about 1.45, monotone, one integer
multiply — which is what every unhinted renderer does to put the weight back.

The font is *compiled in* (`fontdata.c`, generated by `tools/mkfont`), which
answers the original objection completely. There is no file to find and fail
to find; it scales to any size; and the same bytes go through the same integer
code on a Linux runner, under qemu and on a phone, so a frame containing text
is still checkable by checksum. That last part is not a detail — text is most
of what is in a dialog, and a font that rounded differently per platform would
end the ability to check anything about one. The three faces are subsets of
Liberation Sans, its bold, and Liberation Mono, under the SIL Open Font
License; 16 KB each after dropping the hinting bytecode nothing here runs.
Metric compatibility is what actually matters: every control on a dialog is
positioned in units derived from the average character width of a
proportional font, so a substitute with the wrong metrics moves every control
on the page.

## Pictures

`image.c` decodes what a program carries: packed DIBs at 1, 4, 8, 16, 24 and
32 bits, with palettes, `BI_BITFIELDS` channel masks and both row orders; the
icon container, whose header height covers the colour rows *and* the 1-bit AND
mask stacked under them; `.ico`/`.cur` files; and `RT_GROUP_ICON` directories,
searched for the size actually being asked for so a 16-pixel title bar gets
the 16-pixel drawing rather than a shrunk 256.

One case is worth naming because it is the most common way an icon disappears:
a 32-bit DIB's fourth byte is alpha only if something in it is not zero, and
plenty of tools write `BI_RGB` 32-bit with the high byte left at zero.
Honouring that literally makes the whole image invisible.

`LoadImage`, `LoadBitmap`, `LoadIcon` and `LoadCursor` used to return a
plausible handle and no pixels, which is why an installer came up with a blank
banner, an empty title bar and no icons: the program asked, was told yes, and
drew nothing. The stock cursors and the message-box symbols are drawn here
rather than decoded, because they belong to the system and are in nobody's
resources.

## The common controls, and the licence page

`comctl32` registered `SysListView32`, `SysTreeView32`, `SysTabControl32`,
`msctls_statusbar32`, `msctls_trackbar32`, `SysLink` and the rest from the
beginning — a program that cannot create one does not get as far as drawing
anything — but nothing painted them, so an installer's component list and its
tabs were empty holes. Each now has a painter and the messages that fill it:
`LVM_INSERTITEM` / `LVM_INSERTCOLUMN` / `LVM_SETITEMTEXT` and the
`LVIS_STATEIMAGEMASK` encoding that carries a component's tick, `TCM_*`,
`SB_SETTEXT`, `TBM_*`, and `WM_NOTIFY` back to the parent when one is clicked.

The licence page needed two more things. A RichEdit is filled by
**`EM_STREAMIN`**, which hands over a callback and expects the control to pull
the text out of it in chunks — so this calls back into the guest through
`w32_call_guest` until the callback reports a short read. What comes back is
usually RTF, and rendering RTF properly is a project of its own; stripping it
is not, and the sentences somebody has to read before clicking *I Agree* are
the part that matters. And the window text had to stop being 256 bytes, which
is plenty for a caption and nowhere near enough for a licence agreement.

Scroll bars exist for the same reason: a licence is taller than its box, and
with no bar there is nothing to say the text continues and nothing to move it
with. The thumb's size is the visible fraction, the wheel scrolls, and the
extent comes from the painter, because the height of wrapped text is not known
until it has been laid out.

## Visual styles

Windows has drawn its controls two ways since 2001, and only the older one was
here. A program with a comctl32 version 6 manifest — every program built in
the last twenty years — expects flat fills, a thin outline, a soft gradient,
and a blue edge on whatever has the keyboard.

Nothing reads a `.msstyles` file: those are PE files full of somebody else's
bitmaps and they are not ours to ship. What is drawn is the *shape* of the
modern look, which is what makes it read as current, and it goes through
`bevel()` and `button_face()` so switching between the two looks is one branch
rather than fifty. `uxtheme.dll` answers the questions a themed program asks —
`IsAppThemed` and `IsThemeActive` say yes, so it stops drawing its own
Windows 95 fallback, and `OpenThemeData` returns a handle so its themed path
is the one it takes.

## Menus, and the pointer

Two things a person looks for before anything else, and neither was drawn.

A menu used to be a handle with nothing behind it. `CreateMenu` returned a
distinct number, `AppendMenu` said yes, `GetMenuItemCount` honestly said zero,
and nothing appeared or could be clicked. That is survivable for an installer,
which has no menu bar and asks for its system menu only so it can grey out
Close; it is the whole program for something whose File/Options/Help bar is
how anything is reached.

The store is a fixed array of menus, one entry per menu that exists, the same
shape the list-box store has -- and a bar and each of its popups are separate
menus, because that is how Windows models them and how a resource stores them.
`LoadMenu` reads both resource formats for the same reason `DialogBoxParam`
reads both dialog templates: the original is a header, then items each with a
flags word, an id word for anything that is not a popup, and a NUL-terminated
UTF-16 string, with `0x80` marking the last item at a level and `0x10` marking
a popup whose own items follow inline; `MENUEX` is the same tree with wider
fields and DWORD alignment, and it is what a resource compiler emits the
moment the script uses anything added after Windows 95. Reading only the first
would give a menu bar that appears for old programs and not for new ones.

**A menu bar takes room out of the client area, exactly as the caption does.**
That is the same argument the caption's comment makes and it is not cosmetic:
a window's contents are positioned from the client origin, so a bar drawn over
the client area covers the first row of them. `SetMenu` and `DrawMenuBar`
recompute it, because a menu can arrive long after the window did.

**A popup is not a window.** It is drawn as an overlay above everything, which
means it cannot go through a window's device context -- whatever painted next
would draw straight over it. So the popup and the mouse pointer are composited
into the surface immediately before it is handed to the display and lifted
straight back out again afterwards, and the surface a window paints into only
ever holds what the windows drew. The other way round -- leaving them in and
repainting what was underneath when they move -- is what a compositor with a
backing store per window does; here there is one shared surface and no backing
store, so "repaint what was under the pointer" means asking every window that
overlaps it to paint itself again, which is a guest callback for every pixel of
mouse travel. Saving the rectangle and putting it back cannot leave a trail at
all, which is the property that actually matters.

While a menu is up it takes the mouse and the keyboard away from the windows
under it. That is not tidiness either: the click that dismisses a menu must
not also press whatever it landed on. Clicking a bar item drops its popup,
hovering moves the highlight, an item that opens another menu opens it on
hover, clicking an item sends `WM_COMMAND` with its id down the same path a
button's click takes, Escape closes one level and clicking anywhere else
closes the lot. `TrackPopupMenu` runs its own message loop -- it has to, since
`TPM_RETURNCMD` means the call does not return until something is chosen --
and gives everything the menu did not want to the window it was addressed to,
so paints and timers still happen behind an open menu. It is bounded the same
way `GetMessage` is, because with nobody there to click anything a headless
run has to end rather than wedge a CI job. Alt opens the first bar item, the
arrows move, Enter chooses.

### The pointer, and the other pointer

`SetCursor` had been storing a real image handle and `LoadCursor` had been
returning real pixels, including the stock shapes `image.c` draws itself,
since the picture decoding landed. Nothing put them on the screen. Now the
pointer is composited last of all, at the cursor position *less its hot spot*
-- the hot spot is the pixel the coordinates refer to, an arrow's tip or a
cross-hair's centre, and ignoring it puts the picture a dozen pixels from what
is being clicked. `ShowCursor`'s counter is honoured, and that is not a
nicety: a game that hides the pointer is saying it will draw its own, and
drawing this one as well gives it two. A program that has set no cursor gets
the stock arrow, because a window with no pointer over it looks broken.

**The app draws its own pointer overlay, and the two must never both be
visible.** The app's is for touch -- a finger covers what it is aiming at, so
something has to say where the tap will land. This one is the pointer a
program *sets* and a mouse moves. Two arrows a few pixels apart are worse than
none, because neither is obviously the real one, so the app asks
`w32_cursor_visible()` before drawing its own and leaves the drawing to this
one when a real mouse or trackpad is attached.

One limit worth stating: the pointer is composited onto the *surface*, so a
guest presenting its own Direct3D frames does not get one -- those frames
never touch this surface, which is also why moving the mouse does not present
an empty desktop between them. A game that wants a pointer over its own
rendering draws it itself, which is what a game does anyway.

`dlgtest`'s recorded frame checksum covers this: it changed when the pointer
started being drawn, and it is the same number in both bitnesses, which is the
only reason a checksum is worth recording.

## The message pump, and why a game was being killed

`GetMessage` used to return `WM_QUIT` the moment the queue was empty, on the
reasoning that there was no other thread to produce input. That was true of
the command-line runner and stopped being true the day the app existed. An
installer that reaches its first page and waits for a click has an empty
queue, and telling it to quit — or leaving its loop to spin on `PeekMessage` —
is either an early exit or a core held at 100% until iOS terminates the
process. Two of the four crash reports that led here were exactly that:
`cpu_resource_fatal`, 99% CPU over 48 seconds, against a limit of 80% over 60.

So `GetMessage` waits on a condition variable now, signalled whenever anything
is queued. With nobody there to touch anything — the test suite, a headless
run — it gives up after fifteen seconds and returns `WM_QUIT` as before, so a
guest still terminates rather than hanging a CI job. `PeekMessage` is paced
the same way, but only while nothing is happening: presenting a frame or
queuing a message resets the counter, so a game rendering flat out is never
slowed and an idle one stops holding a core.

`OutputDebugString` went the same way. A program calling it in its main loop —
and installers do — wrote a line per iteration, and on a phone each of those
crosses into the system log and wakes the host app's UI. That was enough on
its own to hold the main thread at 100% until iOS terminated the process,
which is a strange way for a debug print to kill a game.

## Sprites: the D3D9 texture path

The version of `d3d9.c` that drew triangles accepted exactly one vertex
format — `D3DFVF_XYZRHW | D3DFVF_DIFFUSE`, an untextured position already in
screen space — and refused everything else with a line on stderr. That was the
right first step, because it isolated vertex fetch, primitive assembly and
rasterization from the transform pipeline. It also meant no real 2D game drew
anything, because **a sprite is a textured quad** and every engine that puts
one on screen sends `D3DFVF_TEX1` with it.

Worse, `CreateTexture` was not in the device's vtable at all. An entry that is
not there is not a no-op: the call lands on the unimplemented-slot stub, which
stops the run. So a game died during setup, having produced no frame.

What is there now: `IDirect3DTexture9` with `LockRect` (honouring a
sub-rectangle, because a texture atlas is filled one sprite at a time),
`GetSurfaceLevel`, `SetTexture`, `IDirect3DIndexBuffer9`, indexed draws,
render targets with `StretchRect` and `ColorFill`, `SetScissorRect`,
`SetTransform` and the world/view/projection multiply, and the blend states
that matter — `D3DRS_ALPHABLENDENABLE` and the two factors, which between them
say whether a sprite is composited or added.

Vertex fetch is now driven by the format rather than being one shape:
`fvf_decode` walks the FVF in the order the specification fixes (position,
blend weights, normal, point size, diffuse, specular, texture coordinate
sets), and a **vertex declaration** — what replaced the FVF, and what a
2015-era engine actually uses — is parsed into the same layout, so one fetch
serves both. A declaration wins over the FVF when both are set, because an
engine that moved to declarations often leaves a stale FVF behind.

The pixels go through `w32_d3d11_triangle`, the same integer rasterizer D3D11
draws with. It already did texturing, colour modulation and alpha blending and
it is deterministic by construction, so pointing D3D9 at it gives two APIs, one
rasterizer and one set of frame checksums. `d3dtex.exe` draws a wrapped
checkerboard quad modulated by vertex colour, a triangle through the transform
pipeline, a blended quad and an indexed draw out of real buffers; the frame is
`4cbae1e7` on x86, under qemu on aarch64, and in both bitnesses.

**Shaders run** (since September 2026). `CreateVertexShader` and
`CreatePixelShader` decode the token stream — vs_2_0 to vs_3_0, ps_2_0 to
ps_3_0: the arithmetic, the matrix macros, if/ifc/else, loop/rep/break,
call/ret, relative addressing through `a0` and `aL`, def/defi/defb, dcl,
texld with its projected and biased forms, texldl, texkill — and a draw with
a shader bound goes through `win32/d3d9_shader.c` per vertex and per pixel.
The vertex is fetched element by element as its declaration says (or as the
FVF implies), so every `D3DDECLTYPE` a declaration can name is read; the
constants set with `Set*ShaderConstant{F,I,B}` are what the shader sees, and
a shader's own `def`s are loaded when it is bound, as the hardware does. A
vertex shader alone runs in front of the fixed-function texture-times-colour;
a pixel shader alone runs behind the fixed-function transform; all sixteen
texture stages and their sampler states are visible to it, and the alpha test
(`D3DRS_ALPHATESTENABLE`) is honoured on this path. ps_1_x, vs_1_1 and
predication are refused by name, in which case the fixed-function reading
draws and the report says which shader and why. `tests/test_sm3.c` checks
the interpreter instruction by instruction on hand-assembled tokens;
`d3dshader.exe` draws three quads through it and is checked by frame checksum.

### Two bugs worth naming

`CreateTexture` was written at slot 20. It is at 23; slot 20 is
`SetDialogBoxMode` and three slots before `CreateTexture` sits
`CreateVolumeTexture`. The tables are now `_Static_assert`ed against lengths
taken mechanically out of `d3d9.h`, which is how this was found.

And the first indexed draw segfaulted the host at address zero on x64 and
worked perfectly in 32-bit. `StartIndex` is a `UINT`: on x64 it arrives in the
low half of a register and the **high half is whatever was there before**.
Reading it at full width turned an index pointer into `0x17d3fb68fa000`. Every
32-bit argument in this file is masked with `(uint32_t)` for that reason, and
the idiom was already all over the older code — it was new code that forgot it.

### The rest of the interface

Seventy-eight of `IDirect3DDevice9`'s slots had no entry, among them
`SetVertexDeclaration`, `SetVertexShader`, `SetRenderTarget`, `CreateQuery`
and `Reset` — all of which a 2D engine calls before it draws. They are
generated by walking the header's own `DECLARE_INTERFACE_` block, so the slot
numbers and argument counts are the header's rather than anyone's memory. Most
are accepted and do nothing, which is the correct behaviour for a renderer
with no lighting, no palettes and no patches, and is very different from
having no entry at all.

## Both bitnesses, one implementation

A PE32 image gets the 4 GB arena (guest address = base + zext32), a PE32+
image the identity mapping — the two memory models `xrun` established.
Every API implementation is written once against `w32_arg(i)`, `w32_ret()`,
`W32P()` and a few size helpers, so `WriteFile` or `printf` do not know which
ABI called them. `hello32.exe`/`hello64.exe`, `crt32.exe`/`crt64.exe` and
`nbody32.exe`/`nbody64.exe` are the same sources built for both.

Two facts about the host shape guest memory. Guest pages are never executed
by the host — the interpreter reads them, the dynarec translates them — so
they are mapped read/write whatever `PAGE_EXECUTE_*` the image or
`VirtualAlloc` asked for; Apple silicon refuses RWX mappings outside
`MAP_JIT` and iOS refuses them outright (the first Apple-silicon CI run of
this layer died on exactly that: the stub page came back unmapped and the
first `int3` was written to address 0). And the guest's 4 KB pages are a
fiction on Apple silicon, whose kernel maps in 16 KB units and rejects
`mmap(MAP_FIXED)`/`mprotect` on anything less aligned; `w32_alloc*`,
`VirtualFree` and `VirtualProtect` widen guest ranges to host pages
(`w32_host_page()`), and the Linux CI job runs the suite once more with
`WINRUN_HOST_PAGE=16384` so the 16 KB path is exercised on every push, not
only on the Mac.

## Pointers the guest supplied, and where they are checked

The identity mapping has a consequence the arena does not. A PE32 guest's
address is `base + zext32`, so every number it can produce lands somewhere
inside one 4 GB reservation and there is nowhere else for it to point. A PE32+
guest's addresses *are* host addresses. `W32P()` is the identity map for them,
which means any number a program hands to an API becomes a raw host pointer,
and the host then writes through it.

That is not a theoretical hole. A crash report from a device showed

    _platform_memmove -> w32_write -> k_VirtualProtect -> run_loop
    (Data Abort) byte write Translation fault, far = 0x16d08be0

on a guest whose image starts at `0x140000000`. `VirtualProtect` was storing
its old-protection word through an out-parameter the program had never
initialised, and `0x16d08be0` was whatever had been on the stack. On a desktop
that is a segfault and a core file. On a phone it is the whole app vanishing,
with no signal handler in a position to say which of the guest's several
thousand API calls did it — the run report cannot be written, because there is
nothing left to write it with.

The boundary is `w32_mem_ok()` in `tools/winrun/winrun.c`: is this guest range
actually mapped? It answers from the list of mappings the loader made, which
is a few dozen entries, so a linear scan with a one-entry cache costs nothing
at the rate the API layer asks. For a 32-bit guest it is always yes, because
the whole arena is one reservation. `W32PN(w, addr, len)` is `W32P` with that
question asked first and NULL for an answer when it fails, and `w32_read` /
`w32_write` go through it already.

Everything in `win32/` that memsets, memcpys or indexes through an address the
*guest* supplied now goes through `W32PN` — roughly seventy call sites: every
`GetDesc` and `GetCaps` that clears a structure, `Map` and `Lock` and
`UpdateSubresource` on the d3d paths, the CRT's block and string functions,
`ReadFile`/`WriteFile`, the registry value copies, `MultiByteToWideChar` in
both directions. Where the length is variable the whole range is checked
rather than the first byte, and the arithmetic is done in 64 bits: a
`SysMemPitch` times a height, or a `PrimitiveCount` times three, is exactly
the product that wraps in 32 bits to something small and plausible.

Addresses this side computed are still plain `W32P`. A buffer that came back
from `w32_alloc` or `w32_heap_alloc` a moment earlier, a texture's pixels read
out of a COM object we made — checking those would buy nothing and would put a
scan on the inside of the rasterizer's loops.

A failed check does not fault and does not silently succeed: each call site
returns what that API returns when it cannot use a pointer. `E_INVALIDARG` and
`D3DERR_INVALIDCALL` for the Direct3D structures, `ERROR_NOACCESS` for a Win32
output buffer, `ERROR_INVALID_USER_BUFFER` for a file read or write,
`ERROR_INVALID_ADDRESS` for a range `VirtualProtect` or `VirtualFree` was
asked to operate on, `TIME_ZONE_ID_INVALID` from `GetTimeZoneInformation`,
NULL from the CRT's `memcpy`. Where the function returns void — `GetDesc` on a
D3D11 texture, `GetSystemInfo`, `UpdateSubresource` — there is no status word,
so the write is simply not done.

`VirtualProtect` and `VirtualFree` needed more than their out-parameters
checked. Their `mprotect`, `munmap` and `MAP_FIXED` calls act on whatever is at
the address, and for a 64-bit guest that is a host address: a guest could have
made the host's own text writable, or unmapped libc, by passing a plausible
number. Those now refuse a range that is not the guest's. `IsBadReadPtr` and
`IsBadWritePtr` used to answer from `W32P`, which says yes to every non-zero
number — so a program using them precisely to avoid a fault was told to go
ahead and fault. They answer from the mapping table now.

Four things are deliberately still unchecked.

The guest's own instructions. A program that dereferences a wild pointer in
code the compiler inlined — the `memcpy` that mingw turns into a `rep movsb`
rather than a call — faults inside the interpreter or the dynarec, which have
their own bounds handling and their own recovery stubs. This layer defends the
calls a program makes into the host, not the loads and stores it issues
itself, and the two failure modes look quite different in a report.

C strings with no count. `w32_str`, `GSTR` and `w32_wcslen` scan to a
terminator, and there is no length to check before the scan finds one. Those
sites check a single byte, which establishes that the pointer is in guest
memory at all and catches the uninitialised-local case that motivated all of
this; a pointer that lands inside some *other* mapping and runs off the end of
it is still a fault. A properly bounded scan belongs in the loader next to
`w32_mem_ok`, where the extent of the containing mapping is known.

Fields read back out of the objects we make. A COM object's state lives in
guest memory, so a program can scribble on its own texture's width — and then
the rasterizer walks off the end of a buffer whose size it thought it knew.
That is a different problem from a wrong pointer and wants a different answer,
probably validating the field rather than the address.

And 32-bit guests, where every check compiles to the same `return 1` and the
arena makes the question meaningless.

## What the process looks like from inside

TEB (gs on x64, fs on x86) with stack limits, Self, ClientId, LastErrorValue,
TLS slots and ThreadLocalStoragePointer; PEB with ImageBaseAddress,
ProcessHeap, NumberOfProcessors and OS version 10.0.19045. Static TLS from the
image's TLS directory. A 1 MB stack whose bottom frame returns into an exit
stub. `C:\xcore\` is the directory the executable came from; the command line
is quoted the Windows way; an environment block with the usual variables.
Three standard handles (4, 8, 12) onto the host's stdio.

`kernel32`: process and module queries, `GetProcAddress`/`LoadLibrary` onto
the built-in DLLs, memory (`VirtualAlloc/Protect/Query/Free`, heaps), TLS,
critical sections and SRW locks as no-ops (one thread), files and console,
time, code pages and the MultiByte/WideChar conversions, `IsProcessorFeature-
Present` reporting SSE through SSE4. `msvcrt`: the CRT startup protocol
(`__getmainargs`, `_initterm`, `__set_app_type`, `_onexit`...), heap, the
string family on guest memory, stdio on the standard streams, and the printf
family with a formatter that walks the *guest's* va_list (8-byte slots on x64,
4-byte slots with 8 for doubles and 64-bit integers on x86) and formats each
conversion through the host. `VirtualProtect` tells the block cache to drop
compiled code for pages that become writable.

## Verified

`tests/win32/run.sh` runs forty-four checks over seventeen programs and
compares stdout and exit code with recordings: the three-import `hello`, the
full mingw-w64 CRT program (`crt.c`: TLS callbacks, `__getmainargs`,
`_initterm`, malloc/free, `sqrt`, `printf`, `snprintf`, exit code), the n-body
benchmark, the loader test `dlltest`, the four Direct3D 9 programs `d3dtest`,
`d3dframe`, `d3dloop` and `d3ddraw`, the file-layer tests `pathtest` and
`filetest`, the registry test `regtest`, the exception tests `sehtest` and
`faulttest`, the window-and-input test `inputtest`, and the audio and gamepad
test `audiotest`, and the threading test `threadtest` — each as PE32 and PE32+.
Four of them are run more than once: `regtest` twice per bitness because what
it tests is what survives between two runs, and `faulttest` and `threadtest`
both with the dynarec and without it, because "the same either way" is the
property that matters there — for `faulttest` because a fault inside a
compiled block takes a different path, and for `threadtest` because the two
engines hand the guest lock over at different granularities and so interleave
differently.

`d3dtest` calls Direct3D 9 the way a game starts up — `Direct3DCreate9`,
`GetAdapterIdentifier`, `CreateDevice`, `Clear`, `Present`, then
`GetBackBuffer` and `LockRect` to read the pixels back — and every one of
those goes through a COM vtable, so it is also what keeps the slot numbers
honest. `d3dframe` produces an actual picture: it locks the back buffer and
draws a gradient, a disc and a checkerboard with integer arithmetic (so PE32
and PE32+ produce byte-identical output), presents it, and prints an FNV-1a
checksum of the frame. `d3dloop` is the same thing as a loop — the shape a
game's frame loop has — animating a disc across the screen and either stopping
at a frame count (which is how it is tested) or running until the device is
lost (which is how the app runs it). All are recorded like the others, and the
same checksums come out of the interpreter, the qemu JIT and the device.

`dlltest` is built as a chain — `dlltest.exe` statically imports `mid.dll`,
which statically imports `sub.dll`, and `late.dll` is in nobody's import table
and reachable only through `LoadLibrary`. Each DllMain writes its name into a
log inside `sub.dll`, so the program prints the order the loader actually used
(`sub mid exe`, then `late` when it is loaded) rather than trusting a return
code. It also checks `GetProcAddress` by name and by ordinal against each
other, that a DLL imported twice is one image and not two, that a forwarded
export lands on the real function in the other DLL, that a name that is not
exported gives NULL, and that the module stays callable after `FreeLibrary`. The 64-bit n-body output is byte-identical to the Linux build of the
same source (`tests/guest/nbody`), which is byte-identical to native x86. The
suite runs on the x86 runner (interpreter, 4 KB and simulated 16 KB pages),
under `qemu-aarch64` (JIT) and natively on the Apple-silicon CI job (JIT,
16 KB pages). `winrun` builds on Linux and macOS and will build for iOS
unchanged: it is plain C over `mmap`. A host crash prints the guest RIP/RSP,
the runtime's map (image, stubs, TEB, stack, heap) and a host backtrace, so a
CI log is enough to start from.

## Measured

`nbody64.exe 300000` through the JIT under qemu: 1.26 s — the same as the
Linux binary, because it is the same SSE2 code. `nbody32.exe`, built i686 so
the compiler emits **x87** for `double`, used to take 102 s (every
FLD/FADD/FMUL a callout to the SoftFloat interpreter). With x87 lowered onto
NEON doubles (see the x87 section of `docs/DYNAREC.md`) it is 2.1 s for
200000 iterations — 43× faster, and within ~2.3× of the SSE2 build. Windows
processes start in 53-bit precision (the mode the lowering handles natively),
which is what `winrun` and a real MSVC CRT both set; Fallout 3 and New Vegas
are 32-bit MSVC programs that do their scalar float on the x87 stack, so this
is the path they run on. `_controlfp`/`_control87` are implemented on the
guest's real FCW/MXCSR so the CRT actually reaches that mode.

## Where a guest's files are

A Windows program opens `C:\...` for what it installed and a bare relative name
for what sits beside its executable, and both have to land somewhere real or a
game cannot find its own data.

- `C:\...` resolves under a **drive root** the host chooses — the app points it
  at its own storage, `winrun` takes `-C dir` or `WINRUN_DRIVE_C`. With no root
  set (the command-line default) an absolute path falls back to being relative,
  which is what the older behaviour was.
- A relative path resolves against **the executable's own directory**, because
  that is the working directory a Windows program is started in. Not the host
  process's cwd, which on iOS points nowhere useful.
- `C:\xcore\...` is still the executable's directory, because that is the
  location `GetModuleFileName` has always reported and the recorded guest
  output depends on it.

`pathtest.exe` locks all three down, including a path that is deliberately not
there — "not found" is a result too, and a file layer that cheerfully opens
anything would pass a weaker test.

### Finding files, and mapping them

A game does not open its data by name. It walks a directory for `*.bsa` and
then maps what it finds, because an archive is larger than it wants to read.
Both of those now work.

`FindFirstFile`/`FindNextFile`/`FindClose` walk a directory against a wildcard
and fill `WIN32_FIND_DATA` — name, size, attributes, times — with the two
failure shapes callers actually branch on: `INVALID_HANDLE_VALUE` and error 2
when the pattern matches nothing at all, and error 18 (`ERROR_NO_MORE_FILES`)
when the walk runs out. Matching is `*`/`?` against the host directory,
case-insensitively, since that is what a Windows program expects.

`CreateFileMapping`/`MapViewOfFile` place the file's bytes in guest memory.
Where the offset is host-page aligned and the host agrees, that is a real
`mmap(MAP_FIXED)` over the guest pages — the file is not copied, which is the
entire point on a device with an 8 GB archive and 6 GB of RAM. Where it is not
(an unaligned offset, or the host refuses), the anonymous pages stay and the
range is filled with `pread`. A guest cannot tell which happened, which is why
`filetest.exe` reads the same file both ways and requires the bytes to agree:
that equality is the only property of a mapping that matters.

Host pages are 16 KB on Apple silicon and 4 KB on the x86 runner, so the
alignment arithmetic goes through `w32_host_page()` rather than a constant, and
the suite runs on both.

## The registry

An installer writes it and the program it installed reads it back, so the only
interesting property is that it survives the process that wrote it: a game asks
`HKEY_LOCAL_MACHINE` where it was installed and takes the answer as gospel.
`advapi32.dll` is therefore a real store, not stubs that return success.

It is deliberately flat. A registry is a tree, but every operation a program
performs names a key by its full path, so a sorted list of
`(key, name) -> (type, bytes)` answers all of them and "does this key exist"
becomes "is any entry at this path or below it". There are no node objects and
no parent pointers, so there is nothing to keep consistent; subkey enumeration
derives the immediate children on demand instead. An empty key gets one empty
unnamed value to mark that it exists, the same trick a filesystem plays with an
empty directory.

On disk it is `registry.txt` in the drive root, one line per value as
`path|name|type|hex`. Text, because being able to read and fix it by hand is
worth more than the bytes, and because a corrupt binary blob would take a
game's install path with it.

Open/create/query/set/enumerate/delete are there for values and keys, in both
`A` and `W` forms where a program uses them, plus `RegQueryInfoKey`. Two details
that callers depend on and stubs usually get wrong: a query with a null buffer
reports the size rather than failing (every caller does this first), and one
with a buffer too small returns `ERROR_MORE_DATA` with the size it wanted
rather than truncating. A wide string is stored narrow, so a program that
writes `W` and reads `A` — installers do both — sees the same value.

`regtest.exe` is two programs in one: `regtest write` creates a key, writes
`REG_SZ`, `REG_DWORD` and `REG_BINARY` values, creates an empty subkey,
enumerates, and deletes one value; `regtest read` opens that key without ever
creating it, reads the values back, confirms the deleted one is gone, and then
deletes the tree so the next run starts clean. The second half is only right if
the store reached the disk. The device build runs the same pair, and because
every run begins by throwing the whole in-memory store away, a value the second
run can see came back off disk and nowhere else.

## Structured exception handling

Both bitnesses are implemented; they work differently. 64-bit is described
after the 32-bit mechanism below.

On 32-bit Windows this is a linked list on the stack. `__try` pushes an
eight-byte record — next pointer, handler address — and stores its address at
`fs:[0]`; `__except` pops it. When something faults, the kernel walks that list
from `fs:[0]` outward, calling each handler with a description of what happened
and a copy of the register state, and the first one that says "I will deal with
this" gets control. All of it lives in memory the guest owns, so implementing it
is a matter of walking the guest's own list and calling the guest's own
functions.

It matters more than it looks. 32-bit MSVC compiles C++ `throw` into
`RaiseException(0xE06D7363)` and catches it through this same chain, so a game
built with MSVC has this on its critical path without containing a single
`__try`. It is also how a program survives its own bad pointer, which is why
code that works on Windows dies without it.

`win32/seh.c` has the dispatcher, `RaiseException`, `RtlUnwind`,
`SetUnhandledExceptionFilter`, the vectored handlers, `RtlCaptureContext`,
`NtContinue` and `IsBadReadPtr`. What a handler is handed has to be laid out
exactly right or it reads the wrong fields: `EXCEPTION_RECORD` (80 bytes on
x86, 152 on x64) and `CONTEXT` (716 / 1232). Those offsets were read out of
mingw-w64's headers with `offsetof`, by a program compiled for Windows and run
on this emulator — a pleasant way to get them from the source of truth rather
than from memory.

Two details callers depend on and stubs usually get wrong: the search order is
vectored handlers, then the frame list, then the unhandled filter; and a
handler returning `ExceptionContinueExecution` gets the whole `CONTEXT` copied
back, not just the fields we expect it to have touched — a handler that repairs
one register and resumes has to actually resume with that register repaired.

`RtlUnwind` is what makes `__except` work rather than just the search: it pops
the list down to the target frame, giving every handler it passes a chance to
run its `__finally` blocks (`EXCEPTION_UNWINDING`), then returns to the caller,
which jumps into its own handler body.

### Faults, and where the guest's registers were

A CPU fault becomes the exception Windows would have raised for it: an access
violation with the address in `ExceptionInformation[1]`, a divide error, a
stack overflow when the address is just past the end of the guest stack.

The interesting part is that this works from dynarec-compiled code, where it
has no right to. The compiler keeps guest registers in host registers and
writes them back at block boundaries, so a fault in the middle of a block has
no consistent guest state to report — and a `CONTEXT` built from the last
block exit would be quietly, plausibly wrong, which is worse than failing.

Reconstructing the state in C would mean a second implementation of everything
the compiler's spill knows — which host register holds which guest register,
the x87 renaming, the tag word, the pending FPSR fold — kept in step with the
first by hand, in the one place where a disagreement is hardest to notice. So
the generated code does it instead. Every guest memory access gets an
out-of-line *recovery stub* carrying exactly the spill sequence for that point
in the block; a host `SIGSEGV` at one of those instructions is turned into a
jump to its stub, which writes the registers back and leaves through the
dispatcher. `xc_run` returns with a cpu struct as consistent as if the block
had ended there.

The lookup is an exact match on the host PC, and that is what makes it safe to
do at all. It is not a guess about where a fault came from: the PC either is
one of the load/store instructions the compiler emitted for a guest access, or
it is not, and a fault anywhere else stays a crash with a report. Nothing is
added to the fast path — the cost is code size, about 15–20% more generated
code in the programs measured, and only in blocks that touch memory.

`tests/test_faultdiff.c` is what holds this honest. x86 says a faulting
instruction has no effect — including a faulting `push`, which leaves the stack
pointer alone — so the architectural state at a fault is exactly the state
after the instruction before it. That is the oracle, and the interpreter can
produce it without faulting: run it with a step budget one short of the
faulting instruction and compare every register, every XMM, the flags, the
whole x87 stack and tag word, the reported RIP and guest memory against what
recovery reports. Thirty-two sequences (generated from real assembly by
`tools/gen/faultcases.sh`, each carrying its own disassembly) put as much state
in host registers as possible first: dirty GPRs, dirty XMMs, a loaded x87
stack, a stack pointer the lowering has already moved, a register cache a
callout has just invalidated.

Stating it as a claim about x86 rather than as agreement between two
implementations is the point — two implementations can be wrong together. It
caught a real one immediately: the dynarec drops a flag computation nothing in
the block reads, which is sound right up until a fault hands control to a
handler the liveness pass never saw. An instruction that can fault is now a
reader of the flags.

## A window, a message pump, and input

A game's first act is to register a window class, make a window, and then
loop: pump messages, read the keyboard and mouse, draw a frame. `win32/user32.c`
is that.

A window is host-side state behind an HWND the guest holds — the same trick COM
objects use. Its WndProc, though, is *guest* code, so `DispatchMessage` calls
back into the guest through `w32_call_guest`, the path TLS callbacks and
`DllMain` already take. `WM_CREATE` goes straight to the WndProc rather than
through the queue, because a program is entitled to have run it before
`CreateWindowEx` returns and plenty of them set things up there.

Structure layouts (`WNDCLASS`, `WNDCLASSEX`, `MSG`, `DEVMODE`, `CREATESTRUCT`)
were read out of mingw-w64's headers with `offsetof`, by a program compiled for
Windows and run on this emulator. A wrong WndProc offset is a jump to whatever
was next in the struct, so these are not the place to work from memory.

### Two ways in, because games use both

Messages are the queue. But a game's frame loop mostly does not read them: it
asks `GetAsyncKeyState` whether W is down *right now* and `GetCursorPos` where
the mouse is. So an injected event does two things — updates the state array
immediately, and queues a message. The state is what a frame loop sees, the
queue is what a message loop sees, and a program using either sees the same
events. `inputtest` checks both against each other on every frame.

Relative motion is kept apart from the pointer position, because mouselook
needs deltas that keep coming when the pointer is against the edge of the
screen, and `SetCursorPos` — which a game calls every frame to recentre —
must move the position without looking like motion, or the view spins.

### Where input comes from

    void w32_input_key(int vk, int down);
    void w32_input_key_ch(int vk, int down, uint32_t ch);
    void w32_input_char(uint32_t ch);
    void w32_input_mouse_move(int x, int y);        /* absolute, client pixels */
    void w32_input_mouse_delta(int dx, int dy);     /* relative */
    void w32_input_mouse_button(int button, int down);
    void w32_input_mouse_wheel(int delta);

The app's key, pointer and touch handlers call these from the UI thread while
the guest runs on its own, so the state and the queue are behind a mutex and
nothing on the injection side touches guest memory — there is no `w32 *` in any
of these signatures, and that is the point.

`w32_input_key_ch` carries the character the host's keyboard layout resolved.
It is recorded against the key rather than queued as a `WM_CHAR`, because
Windows produces `WM_CHAR` inside `TranslateMessage` and a program that never
calls `TranslateMessage` is entitled to never see one — queueing it as well
would give a text field two of every letter. `TranslateMessage` uses the
host's character when there is one and derives a US-layout one otherwise.

Coordinates are client pixels of the guest's window. The app knows the rect it
drew the last frame into and maps a touch through it, so the scaling lives in
one place; `w32_client_size()` is that size, and `w32_cursor_visible()` tells
the app whether the guest wants a pointer drawn at all.

### On the device

Three sources, in the order a game would prefer them:

- **A hardware keyboard**, through `pressesBegan`/`pressesEnded`. `UIPress`
  gives the raw HID usage *and* the layout-resolved character, which is exactly
  the pair Windows wants. The HID-to-virtual-key table is written as raw HID
  numbers with the names in comments: the numbers are the wire format, and a
  table of them can be checked against the HID spec in one pass.
- **A hardware mouse or trackpad**, through GameController's `GCMouse` — the
  only iOS API that reports relative motion, which is what mouselook needs.
- **The touchscreen**, when there is neither. Dragging moves the cursor the way
  a trackpad does, relatively, rather than teleporting it under the finger:
  the finger would cover what it was aiming at, and a game that has hidden the
  cursor wants deltas anyway. Tap is a left click, two-finger tap a right
  click, and a long press holds the button down so a drag can drag. A small
  hold-to-press key overlay covers WASD and the few actions a game needs when
  no keyboard is attached.

The guest says which mode it is in without being asked: a game that calls
`ShowCursor(FALSE)` is saying "I am reading relative motion now", so the
crosshair gets out of the way then and comes back for menus.

### Testing input, which needs a driver

    winrun -input script.txt program.exe

Each line is `[frame N] <event>`, with the events being `key down|up <vk>`,
`char <n>`, `mouse move|delta <x> <y>`, `mouse down|up left|right|middle` and
`mouse wheel <delta>`. A frame is a `Present`, so "frame 3" lands where a real
event would arrive in the middle of a frame loop.

That matters because a recording of the tester's reflexes is not a test.
`tests/win32/inputtest.script` aims events at particular frames and
`inputtest.expected` says what each frame saw — byte-identical between PE32 and
PE32+, and between the interpreter and the dynarec. The device diagnostics runs
the same script through `win_probe_run_script`, so what CI checks and what the
device checks are the same events. The guest also exits non-zero if no event
reached it at all, so a harness that only looks at the exit code cannot pass on
a run that received nothing.

The script is also how to reproduce an input bug: the script *is* the repro.

## Audio that initialises, and a gamepad

Neither of these makes a sound or needs a controller plugged in, and both are
still worth having, for the same reason: the thing they prevent is a game
*refusing to start*.

`dsound.dll` exists because a great many games do not degrade to silence when
audio init fails — they abort. `DirectSoundCreate8` returning an error is the
end of the program, so "no audio" and "no audio subsystem" are very different
amounts of broken, and this is the second one.

A sound buffer really is guest memory of the size asked for, and `Lock` hands
back a pointer into it. That matters more than it sounds: a game writes its
mixed audio there and then reads its own *play cursor* to decide how much more
to write. So the cursor advances at the rate the buffer's format implies, from
a clock, and a program waiting to be told "you may write the next 4 KB" is told
so on time. Get that wrong and a game that would have been merely silent
instead hangs in its audio thread. `Lock` also returns two pointers when the
region crosses the end of the circular buffer, because a program handed only
the first writes past the end.

That last step — the bytes reaching a speaker — exists since September 2026:
`audio_out.c` is one integer mixer over *sources* (a DirectSound buffer, a
`PlaySound` clip, an XAudio2 voice's block), feeding an `AudioQueue` on macOS
and iOS, and pumped by the wall clock into nowhere when no device is open so
that nothing waits forever on a machine without a speaker. `test_audio.c`
checks the mix to the sample. DirectSound buffers keep their format, honour
`SetCurrentPosition`, end when played once, and fire their notification
positions from the guest thread at every wait; `xaudio2.c` is the queue-per-
voice API on top of the same mixer, in both the 2.7 and the 2.8/2.9 layouts.

`QueryInterface` on a buffer is real here, unlike the shared one COM objects
use by default. A game asks a buffer for `IDirectSound3DBuffer` and then calls
`SetPosition`, which is slot 17 there and `GetFrequency` on the buffer —
returning the same object would be a call into a differently shaped vtable. The
IID is matched in full rather than by its first word: a loose compare guarding
the one thing worth being strict about would be perverse.

### Vtable slots come from the header now

    sh tools/gen/vtables.sh > win32/vtables_gen.h

A COM interface is an array of function pointers and the *order* is the ABI.
One slot out does not fail to compile and does not fail a test that never calls
it — it calls whatever is next in the table, which is how the missing
`IDirect3DDevice9::CreateDepthStencilSurface` first showed up as "unimplemented
slot 57". So the slot numbers are generated from mingw-w64's own headers as
named constants and the tables index by name: a table with the wrong *name* in
a slot cannot compile, and a missing slot leaves an obvious hole.

### XInput

Eight functions, and the cheapest breadth on the list: every game from about
2006 onwards reads a controller through XInput, and unlike DirectInput there is
no COM, no enumeration and no data formats — a struct of buttons and axes,
polled. All five DLL names games link against (`xinput1_1` through `1_4`,
`xinput9_1_0`) resolve to the same implementation, because the name changed five
times and the functions did not. Ordinal 100, `XInputGetStateEx`, is there too:
undocumented, and imported by ordinal by a fair number of games that want the
Guide button.

It reports a connected pad and feeds it from the same key state `user32.c`
keeps, so a real keyboard and the on-screen keys both drive it. That is not a
stopgap — on a tablet the keyboard *is* the controller for a lot of people, and
a game that only reads XInput would otherwise be unplayable however good the
touch controls were. `w32_pad_state()` lets the host substitute a real
controller's values, and `present` distinguishes "nothing attached" from "a pad
reading zeros", which is what a game uses to decide whether to show controller
prompts at all.

The packet number changes only when the state does. A game polling at its frame
rate uses it to skip work: one that always changes makes every frame look like
new input, and one that never changes makes a game ignore real input entirely.

`audiotest` checks all of it — that every call succeeds, that what was written
to a buffer is what is there, that a lock across the end comes back in two
pieces, that the cursor moves at roughly the format's rate (a wide window: it
is a clock, not a real-time guarantee), and that injected keys arrive as stick
deflection and buttons.

## Threads

`win32/thread.c`. `CreateThread`, `_beginthreadex`, `_beginthread`,
`ExitThread`, `TerminateThread`, thread handles you can wait on,
`GetExitCodeThread`, `SuspendThread`/`ResumeThread`, priorities, critical
sections, events, mutexes, semaphores, the interlocked family, `Sleep`,
`SwitchToThread`, and `TlsAlloc`/`TlsGetValue`/`TlsSetValue`. Nearly every
game written after about 2000 starts a thread in its first second — a loader,
an audio mixer, a decompressor — and until this landed each of them failed at
that line.

A guest thread is a real `pthread`. What it is not is a thread that runs at
the same time as the others: **one thread executes guest instructions at a
time**, holding a single lock that is handed over between execution slices
(`SLICE` is 2^18 instructions) and at every call that would block. So there
is real concurrency of *waiting* — a thread inside `WaitForSingleObject` is
not on the CPU, and a game whose loader thread blocks on a file read is
correctly overlapped with its main thread — and no concurrency of *executing*.

That is a deliberate trade, and worth being explicit about because it is the
kind of thing that reads as an oversight. Everything the emulator itself owns
is shared, mutable and was written single-threaded: the block cache, the code
arena, the guest heap, the handle table, the module list, the recovery-stub
side table. Making those individually thread-safe is a large amount of work
whose payoff is throughput; making the *guest's* threads correct is a small
amount of work whose payoff is that programs run at all. The big lock buys the
second immediately and leaves the first as a later optimisation. What it costs
is parallel speedup — a performance ceiling, not a compatibility one — and a
guest that busy-waits on another thread without ever calling into the API
would spin for a full slice before yielding.

### What is per-thread and what is not

Registers, the TEB, the stack, the last-error value and the TLS slot
*contents* are per thread. Guest memory, the heap, handles, the module list
and TLS *index allocation* are per process. Writing it down that way is what
made the change small: the fields that had to move out of `w32` were
`teb`, `stack_base`, `stack_limit`, `last_error`, `depth`, `tls_array` and
`tls_slots`, and they were **deleted** from the struct rather than left in
place, so every site that still used the old ones failed to compile instead of
silently reading the main thread's copy. TLS values needed no work at all —
they already lived in the TEB, and the TEB was already per-thread-shaped.

Each thread gets its own `xc_cpu`, its own stack and its own TEB with the
stack bounds filled in, because the CRT probes them and 32-bit SEH checks that
a registration record lies inside them. Thread ids are `5000 + slot`.

### The bug, and why the test is written the way it is

The first run of `threadtest` under load reported a guarded total of 99997
against an interlocked total of 100000. Both counters are incremented the same
number of times by the same threads; the guarded one is protected by a
critical section and the interlocked one by `InterlockedIncrement`.

Disassembling the guest settled it: `InterlockedIncrement` had compiled to a
native `lock addl $0x1,(mem)` — one instruction, so no interleaving can split
it, so it cannot lose an update no matter what the locking does. The guarded
increment is three instructions (load, add, store) and can. So the failure was
not in the counters; it was that the critical section was not excluding
anything.

The cause: **every guest thread took the guest lock, and the thread the
process started on did not.** `w32_run()` on the main thread had never been
bracketed by `w32_guest_lock()`/`w32_guest_unlock()`. Two consequences, one
obvious and one worse — no exclusion between the main thread and the workers,
and `pthread_cond_timedwait` being called on a mutex the caller did not hold,
which is undefined behaviour and can leave the mutex in a state where two
waiters both proceed. One `w32_guest_lock()` in `tools/winrun/winrun.c`, around
the main thread's guest execution and including `DllMain` and the TLS
callbacks (which are guest code too), fixed both.

Two things made it findable. The first is that `threadtest` forces the race
rather than hoping for it: inside the guarded read-modify-write there is a
`Sleep(0)`, which hands the guest lock over in the middle of the update. That
turns a three-in-a-hundred-thousand flake into a deterministic loud failure.
The second is that every line `threadtest` prints is true under *every*
interleaving — "four threads each added 400, so the total is 1600", "the ids
are distinct", "both counters agree" — which is what makes a recorded
expectation possible for a concurrent program at all. A test that recorded an
*order* would record the scheduler.

`tests/win32/threadstress.c` is the same increment 25000 times per thread with
no forced handover. It is deliberately **not** in the suite: it is how the
lost update was originally found, and a passing run of it proves nothing.

It is also where I got in my own way for an hour: I checked the output by
grepping it for the lines I expected, and so did not see that
`WaitForMultipleObjects` had timed out and the run was simply incomplete. The
counters I was staring at came from a truncated run. Reading the whole output
instead of grepping it is what unblocked it.

### Verified

`threadtest` runs as PE32 and PE32+, through the interpreter and the dynarec,
on x86-64 (gcc and clang) and on aarch64 under `qemu-aarch64` — both engines
on purpose, because the dynarec hands the lock over at a block boundary and
the interpreter at an instruction, so they interleave differently and only
running both shows the answer does not depend on which. It also runs inside
`test_winrun_lib`, back to back with every other guest in both directions,
which is where a `winrun_reset()` that forgot the thread table or the
per-thread CPUs would show up. `threadstress` has been run sixteen times
across both bitnesses and both engines: 0 lost updates.

## Importing a program from outside

Two files, `tools/import/` and a CLI (`wimport`) so both halves can be tested
on a desktop. The app calls the same code through `win_probe_import`.

A download is one of two things and they need opposite treatment:

- **a game** — already installed, a folder of files, usually inside a zip;
- **an installer** — a program whose *output* is the game.

Getting that wrong is not harmless. Copy an installer in as a game and the
library gets an entry that opens a dialog and stops, and there is no dialog
here to click. So the file is examined first and the likely mode offered as the
default, with the other still one tap away — a detector that cannot be
corrected is worse than one that asks.

### What kind of file is this

`sk_identify` reads the first few megabytes and the last 64 KB and looks for
the marker the builder left: a loader's window class, a self-extractor's own
name, an archive header. Inno Setup, NSIS, InstallShield, an MSI
bootstrapper, 7-Zip / WinRAR / zip self-extractors, Wise and Setup Factory are
each recognised by bytes rather than by name — `setup.exe` says nothing, and a
repackaged installer is routinely renamed to the game's title.

The tail matters for one case and it is worth saying why: a zip self-extractor
has no dependable marker except the end-of-central-directory record, which is
at the end of the file by definition. Reading a gigabyte of payload to find it
would be absurd, so head and tail are read separately.

When nothing matches, the answer splits: a PE that wants to elevate or talks
about installing is *"an installer, family not recognised"* and gets the flags
the common families accept; anything else is *"not an installer"*. Both notes
say they are guesses.

### The window is the part that cannot be drawn

A real installer's UI is comctl32 controls, and comctl32 is not implemented.
That would be the end of it, except that every family in wide use has a silent
mode — put there so deployments can happen without a human clicking Next — and
**a silent installer is a file copier with a registry writer attached**, which
is a shape this runtime can be honest about.

So each family carries its own silent arguments, and the differences are not
cosmetic:

- Inno takes the directory as its own `/DIR="..."` argument and needs `/SP-`
  as well, or it opens a "This will install…" prompt before it ever looks at
  `/SILENT`. `/NORESTART` matters because the alternative is an installer
  trying to reboot a phone.
- NSIS takes `/D=<dir>`, **unquoted, and it must be the last argument** —
  anything after it is treated as part of the path. Quoting it is the single
  most common way a scripted NSIS install lands in the wrong folder.
- InstallShield's switches wrap msiexec's, so the directory goes through `/v`
  as a property — and msiexec is a Windows service, not a library, so this
  family is marked as not expected to work and says so before it runs.
- 7-Zip and WinRAR self-extractors are archive tools, so their arguments are
  archive arguments: an output directory and "do not ask".

A zip payload is never executed at all, whatever the wrapper claims to be. It
is unpacked, which removes every way running it could fail.

### What did it install?

Asking the installer would mean parsing a different script format per family.
The filesystem already knows: snapshot the drive, run the setup, snapshot
again, and the difference is the install. That works identically for Inno,
NSIS, a self-extractor and something never seen before.

Two exclusions, both learned rather than assumed. Temp, because an installer
unpacks itself into it and leaves most of it behind — counting that would bury
the twenty files that matter under two thousand that do not. And
`registry.txt`, because the registry is a file at the root of the drive and
*every* installer touches it, so it would appear in every report as though it
had been installed.

Where the program ended up is the directory the executables landed in, not the
common prefix of every added path: an installer that also drops one file in
`Windows\System32` has a common prefix of nothing at all. If it honoured the
directory it was given, that is used; if it ignored it, the report says so.

### Which executable is the game?

A game folder routinely holds half a dozen: an uninstaller, a crash handler, a
redistributable, a launcher shim, and the program. Offering the alphabetically
first is wrong most of the time and offering a list of eight is only slightly
better.

Names are the weakest signal. The engines leave *structural* fingerprints,
and those are worth far more:

| | why it is reliable |
|---|---|
| `<Name>_Data` beside `<Name>.exe` | a Unity game cannot start without it |
| `<Name>.pck` | Godot's package, named after its binary |
| `data.win` | GameMaker's bundle |
| `Binaries/Win64/…-Shipping.exe` | Unreal's own layout |
| `nw.pak` / `resources.pak` | an NW.js or Chromium bundle |

Then position (top of the folder beats three levels down), then the name the
user knows it by, and only then size — as a weak tiebreak, because a bundled
runtime can be bigger than the game. Against that, two penalties: a name that
is a helper or an installer, and anything inside a redistributable or support
folder. Size is the one signal that would otherwise favour a bundled
`vcredist_x64.exe`, which is why it is worth least.

The chosen executable's own directory becomes the DLL search path (`winrun
-L`), because that is where a program finds its libraries — and for an Unreal
title that is three folders below the one the library shows, so it cannot be
inferred from the entry and has to be carried.

### Will it fit?

The destination is a phone. Running out of space halfway through a 12 GB game
leaves a broken half-install *and* a full disk, and the person then has to
work out which of the two to deal with first — so both modes work out what an
import needs before writing a byte, and refuse with the two numbers when it
will not fit.

An archive is measured by its **uncompressed** total, read from the central
directory rather than by trial extraction. That is the whole reason to measure
at all: `tests/import/compressible.zip` holds 4 MB in about 4 KB, so checking
the download's size against free space would wave through a game a thousand
times too big for the device.

An unknown size is not a refusal. A source that cannot be measured means
"carry on" — a wrong "no" is worse than a run that fails on a full disk,
because the person cannot argue with the first one.

### Paths from outside are not trusted

An archive can name `../../etc/passwd` or `C:\Windows\System32\x.dll`. The
sanitiser works component by component — the only way to do it correctly,
since a filter that looks for the substring `..` rejects `a..b` and accepts
`x/../../y` once a first pass has rewritten it. Drive letters, leading
separators and every `..` component are dropped rather than causing a
rejection, so a slightly odd archive still unpacks and a malicious one lands
entirely inside the destination. `uz_safe_name` is exported and tested
directly, because it should not only be reachable through a real archive.

### What an installer needed that was missing

Silent or not, an installer stopped at the first thing it asked for. The list
turned out to be short and every item is something a *game* wants too, on its
second run:

- `CopyFile`, `MoveFile(Ex)` (including the cross-device fallback an installer
  hits constantly moving files out of Temp, and the delete-on-reboot form an
  uninstaller uses), `CreateDirectory`, `RemoveDirectory`, `SetFileAttributes`,
  `GetTempFileName`, `SetEndOfFile`, the wide halves of the file calls;
- `GetDiskFreeSpaceEx` — a real installer refuses to start if this fails, so
  an emulator where it fails is one where nothing installs. It reports the
  host filesystem's real numbers, because the virtual C: is a directory on it;
- `GetVolumeInformation`, reporting NTFS: an installer that finds FAT32
  refuses to write a file over 4 GB, and some refuse entirely;
- **a real current directory.** `SetCurrentDirectory` used to return success
  without doing anything — the kind of lie that surfaces later as a file not
  found in a place nobody looked. It now moves, fails when the directory is
  not there, and refuses a path too long to hold rather than truncating one
  (truncating is worse: the caller would go on to open something it did not
  name);
- `.ini` files — `GetPrivateProfileString`, `GetPrivateProfileInt`,
  `WritePrivateProfileString` — implemented properly rather than stubbed,
  because the format is simple enough that implementing it costs less than
  explaining a stub, and older games keep their settings in one. Writing means
  rewriting the file, and all three cases are handled: the key exists, the
  section exists but not the key (insert at the *end of the section* — after a
  later header would file it under the wrong one), and neither exists;
- `shell32.dll`, which was not here at all: `SHGetFolderPath`,
  `SHGetSpecialFolderPath`, `SHCreateDirectoryEx`, `IsUserAnAdmin`. An
  installer asks where Program Files is before it copies anything. A *game*
  asks the same questions later and for a better reason — saves, which a
  modern Windows game writes to `Documents` or `AppData` rather than its own
  folder. `CSIDL_FLAG_CREATE` is honoured by actually creating the directory,
  which is what the flag asks for.

`w32_drive_init()` makes the skeleton those answers point at — Program Files,
Windows\System32, Temp, ProgramData, a user profile — so a folder that is
reported is a folder that exists. It is called before an install and never
before an ordinary run: a directory walk of `C:\` is observable, and conjuring
six folders into a drive whose contents a test recorded would change that
recording for no reason.

Two calls are implemented and still refuse: `CreateProcess` and
`ShellExecute`. There is one guest process and the runtime's globals — block
cache, code arena, handle table — are per process. They fail with a real error
*and put themselves in the run report*, because "it tried to launch something"
is a fact worth having, and a caller told "done" would wait for a window that
will never appear. An installer that re-launches itself elevated stops here;
one that shells out to a redistributable carries on without it, which is
usually what you wanted.

### The zip trailer, where a reader can be confidently wrong

Two bugs found by re-reading rather than by use, both in the same place: how
the central directory is located.

The offsets a zip records are relative to where the *zip* starts, which for an
archive appended to an .exe is not where the file starts — so a correction is
needed, and the obvious way to get it is to compute the directory's position
as `eocd - cd_size` and compare with what the archive claims. That is right
until the archive is **zip64**, when the zip64 EOCD record (56 bytes) and its
locator (20) sit between the directory and the trailer. Then every
local-header offset comes out 76 bytes late and the reader inflates whatever
happens to be there. A game over 4 GB, or with more than 65535 files, is
zip64.

So the position is no longer computed, it is *found*: try the recorded offset,
try the two places a directory could begin given the trailer layout, and
accept whichever one actually has a directory entry at it.

The second is the end-of-directory record itself. Its signature can appear
inside an archive comment, and a comment comes *after* the record it belongs
to, so a backwards scan meets the decoy first. Two filters: the declared
comment length must reach exactly the end of the file, and — the only test a
decoy cannot pass by accident — a directory must actually be reachable from
what the record says. A candidate that fails is skipped and the scan carries
on. Getting the second filter wrong is what made the first version report a
real archive as *empty*, which is indistinguishable from an archive that is.

`tests/import/zip64.zip` and `comment_trap.zip` are built for these, by
`make_fixtures.py`, because neither case arises from an ordinary archive:
Python only emits zip64 trailers for a file that genuinely needs them, so that
one is assembled by hand.

### Crossing into Swift

The app calls the importer through `win_probe_import` and `win_probe_look`,
and both take flat scalars and caller-provided buffers rather than the
importer's own structs. That is not tidiness: **Swift imports a fixed-size C
array as a tuple of that many elements**, and `wi_result` carries an 8 KB
report buffer — an 8192-element tuple, which is the sort of thing that makes
the Swift type checker take minutes or give up. Nothing crosses the bridge as
an aggregate, and `tools/import/*.h` is deliberately absent from the bridging
header.

Worth writing down because it is invisible from the Linux side: none of the
Swift is built by any check this container can run, so a mistake there costs a
whole CI round trip on a macOS runner.

### Verified

`test_import` is 67 checks with no guest involved: every family against a
fixture, the flag tables (including that NSIS's `/D=` is last and unquoted),
the path sanitiser against traversal attempts *and* against names that merely
contain dots, a real deflate round-trip out of a self-extracting archive,
executable ranking over Unity and Unreal trees the test builds itself, and the
drive diff.

The fixtures under `tests/import/` are minimal PE headers with each family's
real marker bytes inside them, rebuilt by `make_fixtures.py`. They are not
installers and cannot run — identification only reads bytes, so that is all a
fixture has to be. The exception is `fake_zipsfx.exe`, which has a genuine zip
appended, because "can a self-extractor be unpacked" is not answerable against
a fake one.

`tests/import/install.sh` is the end-to-end half: 26 checks over
`tests/win32/fakesetup.c`, a guest that does what a silent install does in the
order one does it — checks free space, resolves the shell folders, makes
directories, copies files, keeps settings in an `.ini`, reads one back, writes
an uninstall key, and leaves a file in Temp that must *not* be counted. It is
deliberately strict: it **fails** unless it was handed the flags the Inno
family takes, so a broken flag table cannot pass by installing anyway. Both
bitnesses, and the program it installs then loads with nothing missing.

One portability bug worth recording, because of how it was found: `strdup` and
`lstat` are POSIX rather than C, so under a strict `-std=c11` they become
implicit declarations returning `int` — a pointer with its top half missing on
a 64-bit target. The default CMake build asks for `gnu11` and never saw it.
The aarch64 cross-check with `-std=c11` did.

## Surveying a library, not a fixture

    winrun -survey <dir>

`-imports` answers "what does this program need" for one binary. This answers
it for a whole folder, and the difference is the difference between a fixture
and a roadmap: the question a compatibility layer has to keep asking is not
what one program needs but *what would unblock the most programs*, and one
hand-written test guest cannot answer that.

Three outputs, because they answer three different questions:

- every missing function, ranked by how many binaries import it — the build
  order;
- the same totalled by DLL, so a subsystem's cost and its reach are visible
  together ("dsound.dll: six functions, twenty-three binaries");
- the binaries closest to running, fewest-missing first — the shortest path to
  something that works, which is not always the same as the most-wanted
  function.

Each binary is loaded in its own fully reset run, the same path `-imports`
takes, so a corrupt or packed file spoils its own line and nothing else. It also
names the two cases that would otherwise read as "nearly ready": a managed
binary, which imports one function from `mscoree` and needs a whole other
runtime, and a file that could not be read as a PE at all.

One caveat the tool prints itself, because it is the most over-read number it
produces: **"resolves every import" means a program can start, not that it
works.** An import table says what a program asks for, not whether the answer
it gets back is right, and several of these are implemented as a value that is
merely plausible.

## What a program needs that we do not have

    winrun -imports program.exe

Loads the image, resolves every import, and prints the ones nothing could
satisfy — without running an instruction. Each unresolved import already
becomes a stub carrying its own name, so the list falls out of the loader for
free, and it turns "what should we build next" from a guess into a list taken
from the binary. Pointed at a real game, it *is* the roadmap.

`tests/win32/gamelike32.exe` is a fixture for it: a program that does what a
game does in its first few seconds — makes a window and pumps messages, starts
a thread, takes a lock, reads the registry, walks a directory, memory-maps a
file, times a frame, queries the display. It is not in the test suite because
its whole purpose is to name what is missing, and that number is supposed to
change. What it reports today:

    76 imports resolved, 0 missing
    nothing is missing: this program can be run.

It was 17 before the registry, files and memory mapping landed, 11 before the
window and its message pump, and 1 before threads — which is what the number
is for. Everything a game touches in its first few seconds — a window and a
message pump, a thread, a lock, an event, the registry, a directory walk, a
memory-mapped file, a frame timer, the display mode — resolves and works, and
the fixture now runs to the end and prints `gamelike: reached the end`
**without `-k`**.

The number being zero does not make it a test. Its purpose is still to name
what is missing, and it will go back above zero the moment it is pointed at
something bigger; the honest reading of a zero here is "the first few seconds
of a game-shaped program no longer stop on an unimplemented function", which
is a floor, not a ceiling. What one program asks for is not what every program
asks for — that is what `-survey` is for.

## Carrying on past what we do not have

    winrun -k program.exe          # and -t seconds, for one that will not stop

Normally, calling an unimplemented import ends the run: it names the function
and exits 127. That is right for the test suite — a guest that silently
half-works is worse than one that stops — but it is the wrong tool for finding
out what a real program needs, because you learn one name per run.

`-k` logs the call, returns zero, and carries on. One run then names
*everything* the program needed, rather than one name per run. `gamelike32.exe`
no longer needs it — it reaches its last line on its own — but a real game
still will, and the flag is how its list gets taken in one pass.

The obstacle was the x86 calling convention: a stdcall callee pops its own
arguments, and an import table gives a name and nothing else. Guess the count
and the caller's stack is corrupt somewhere far away. `win32/stdcall_args.c` is
generated from mingw-w64's import libraries, which decorate stdcall imports as
`_Name@bytes` — 9817 functions across the DLLs a Windows program is likely to
name. A name that is not in the table still ends the run, because "I cannot
return from this safely" is worth saying rather than guessing. On x64 there is
no callee-pop, so anything can be returned from.

**Returning zero is a lie, and sometimes a consequential one.** Zero means
failure for most of the API but success for some (`RegOpenKeyEx` returns
`ERROR_SUCCESS`), and `FindFirstFile` fails with `INVALID_HANDLE_VALUE`, not
zero — which is why `gamelike` goes on to call `FindNextFile` and `FindClose`
on a handle it should have known was bad. For discovery that is fine and even
useful; it is not a way to run anything for real, and `-k` is off by default
for that reason.

## When something goes wrong

Any abnormal end — a fault, an int3 that is not one of ours, the time limit,
the Stop button — and any clean end that leaned on functions we do not have,
prints a run report: the reason, the guest's registers, the instructions at
RIP, which module RIP is inside and at what offset, the module map with the
stack and TEB, and everything called that is not implemented.

It is written for a clean exit too, because "exited 0 having called nine
functions that returned nothing" is also a diagnosis — and because the person
reading it is usually not the person who ran it. A report that only appears on
a crash is a report you cannot ask for.

`w32_request_stop()` ends a run from another thread; it is checked between
execution slices, so nothing is interrupted mid-instruction. `-t seconds` is
the same thing on a timer, which is what stops a runaway program from wedging
the app.

## What is deliberately not here yet

Audio that actually reaches a speaker (it initialises, and a buffer's contents are right by the time
`Play` is called — CoreAudio is the missing consumer). DirectInput, which is
what Fallout 3 and New Vegas read the keyboard and mouse through; it sits on the
state `user32.c` already keeps, so it is a layer rather than a subsystem. 64-bit `__except` (the 32-bit frame list is implemented; the x64
table-driven mechanism is not — see the end of `win32/seh.c`). GDI beyond the
stubs a message loop needs, and any window decoration: a window here is its own
client area, which is what a fullscreen game wants and not what a windowed
program expects. Drawing reaches the screen but goes through the reference
rasterizer, on every core, rather than Metal.

Installers are a partial answer rather than a missing one: silent mode works,
and the importer runs it and keeps what it produces (see above), but an
installer that insists on its window, hands its payload to msiexec, or
re-launches itself as a second process still stops -- and the first of those
needs comctl32, which is the real gap. And guest threads do not run in
parallel — one executes at a time, which is a speed limit rather than a
compatibility one (see the Threads section).

Within the loader specifically: `DLL_PROCESS_DETACH` is never sent (nothing is
ever unloaded and the process exits without unwinding), `DLL_THREAD_ATTACH`
and `DLL_THREAD_DETACH` are sent to guest DLLs since September 2026 (a C runtime
keeps its per-thread state behind them), delay-loaded imports are left to the
guest's own helper, and `GetProcAddress` answers for every host-implemented DLL
by name and by documented ordinal, not only the first four.

Each of those is a defined next step, not a design gap: the stub mechanism, the
two memory models, the calling-convention helpers and now the module table are
the parts that had to be right first, and they are the same parts the
D3D-to-Metal layer will plug into as `d3d9.dll` / `d3d11.dll` / `d3d12.dll` —
which, now that a guest DLL can be loaded and its exports resolved, is the
next thing to build.

## The next build (September 2026)

Everything on the branch `features/next-build`, in the order it landed and with
where to read more.

- **A next process.** `CreateProcess` and `ShellExecute(Ex)` queue a program
  that winrun runs after the parent ends, each with the parent's flags, its own
  arguments and the directory it asked for; the parent gets a signalled handle
  and exit code 0; the report lists what was queued. Still one process at a
  time — the runtime's globals are per process — but installers that run a
  redistributable and launchers that spawn the game get past it. `kernel32.c`,
  `shell32.c`, the loop at the end of `winrun.c`.
- **`DLL_THREAD_ATTACH/DETACH`**, `DisableThreadLibraryCalls`; `FlsGetValue2`,
  `AreFileApisANSI`, `GetUserDefaultLocaleName` (a GameMaker 2026 game called
  the first 222,476 times and died at rip 0 on the made-up zero).
- **`CoCreateInstance`** names the CLSID it refuses, and a registry lets each
  DLL create its own classes: dsound, dinput, xaudio2 (`com.c`).
- **The CRTs a game may not ship**: System32 is a DLL search root; vcruntime,
  ucrtbase and msvcr* fall back to the msvcrt table when no file satisfies
  them; a shipped copy is still preferred.
- **.NET** is detected by the importer and named in the run report.
- **Sound**: `audio_out.c`, dsound wired to it, `PlaySound`, XAudio2 (`xaudio2.c`).
- **DirectInput** (`dinput.c`): keyboard, relative mouse, keyboard-as-gamepad;
  slots from `tools/gen/vtables.sh`, which can read a directory of headers
  fetched by hand (`MINGW_INCLUDE`).
- **SSE3, SSSE3, SSE4.1, SSE4.2** in the interpreter, CPUID says so, and
  `difftest` builds on a Darwin x86-64 host for the 64-bit half.
- **64-bit structured exception handling**: `RtlLookupFunctionEntry`,
  `RtlVirtualUnwind`, `RtlUnwindEx`, `__C_specific_handler`, the tables a JIT
  registers with `RtlAddFunctionTable`; a handler that unwinds returns through
  the host's own stub so the dispatcher's frames unwind too (`seh.c`).
- **7z archives** (`tools/import/un7z.c`): LZMA, LZMA2, BCJ, BCJ2, Delta,
  stored, encoded headers, self-extractors; RAR refused with the reason.
- **The shaders run** (above), on the CPU.
- **DXGI as a Windows 8+ program sees it** (`d3d11.c`): `GetParent` answers
  with the factory above an adapter or swap chain and the adapter above a
  device (a GameMaker runner asked its adapter for the factory, took the
  `E_NOINTERFACE` it got as "Direct3D is broken", put the HRESULT in a message
  box and exited); the factory is `IDXGIFactory2` with
  `CreateSwapChainForHwnd`, the swap chain `IDXGISwapChain1`, the adapter
  `IDXGIAdapter2` with a quarter gigabyte of video memory in its description,
  the device `IDXGIDevice2`.
- **WASAPI** (`mmdevapi.c`): `CoCreateInstance(CLSID_MMDeviceEnumerator)`, one
  active render endpoint with its property store, `IAudioClient` through
  `IAudioClient3` over `audio_out.c` — shared and exclusive mode alike, event
  driven or polled, `IAudioRenderClient`, `IAudioClock`, the volumes and the
  session control. The event is set from the tick that runs inside every
  wait and `Sleep`, so an audio thread paces itself the way it would on
  Windows. `wasapitest.exe`.
- **D3D9 shaders** (`d3d9_shader.c`, above): vs/ps 2.0 to 3.0 interpreted, the
  declaration-driven vertex fetch, constants, sixteen stages, the alpha test.
- **The rasterizer uses every core** (`d3d11_raster.c`): both APIs gather a
  draw's shaded triangles and hand them over at once; rows are dealt out in
  bands, so the result is what one thread would have drawn. `WINRUN_THREADS=1`
  turns it off. The interpreters' scratch state is thread-local for this.
- **For testing a build**: the run report carries the last 48 API calls before
  the end, a subsystems block (audio device, sources and WASAPI streams,
  DirectInput devices, JIT, rasterizer threads, D3D9 draws through shaders
  versus fixed-function, frames presented and the average frame rate), stderr
  with stdout, and a *Detailed run log* switch in Settings that adds winrun's
  own narration (`WINRUN_VERBOSE`) — including, every sixtieth frame, how long
  the last sixty took, and each shader as it is decoded or refused.

Still not here, in the order it matters: the GPU (both rasterizers are CPU,
now on every core; d12mt on the submodule takes DXIL, so a DXBC/SM3 front end
would come first), a depth buffer, guest threads running in parallel rather
than in turn, instanced draws, ps_1_x and vs_1_1.
