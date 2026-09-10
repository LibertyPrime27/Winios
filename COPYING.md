# Licensing

Winios is **GPL-3.0-or-later**. The full text of the licence is in
[`LICENSE`](LICENSE).

    Copyright (C) 2026 LibertyPrime27 and contributors

    This program is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
    Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program. If not, see <https://www.gnu.org/licenses/>.

## Why it changed from GPL-2.0

Until now this project said GPL-2.0, on the grounds that it was "built on
Boxedwine". That was never true of the code. Boxedwine is not in the tree and
never has been -- it is named in `README.md` and `docs/ARCHITECTURE.md`, and the
README says plainly that we took the idea of a soft MMU and not the code. The
whole history has two authors, `LibertyPrime27` and `Claude`, so there is no
third party whose permission a change would need.

The old licence was also inconsistent with what the tree already contains.
d12mt's shader pipeline uses **SPIRV-Cross**, which is Apache-2.0, and
Apache-2.0 cannot be combined with GPL-2.0-only. GPL-3.0 resolves that: the
Apache-2.0 patent and notice terms are compatible with it. So this is a fix to
an existing conflict, not only a preparation for a future one.

It is also what allows Winios to be combined with
[Madeira](https://github.com/willfaust/Madeira) (GPL-3.0), which is the
direction the project is taking.

## The components, and what they allow

| Component | Licence | Combines with GPL-3.0 |
|---|---|---|
| Winios itself (`core/`, `win32/`, `tools/`) | GPL-3.0-or-later | -- |
| [d12mt](https://github.com/LibertyPrime27/d12mt) (`gpu/d12mt`) | MIT | yes |
| [Zydis](https://github.com/zyantific/zydis) | MIT | yes |
| Berkeley SoftFloat | BSD-3-Clause | yes |
| [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) | MIT | yes |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Apache-2.0 | yes (and **not** with GPL-2.0-only) |
| Liberation Sans / Mono subsets | SIL OFL 1.1 | yes |
| Wine (via Madeira) | LGPL-2.1-or-later | yes |
| FEX-Emu (via Madeira) | MIT | yes |
| DXMT (via Madeira) | LGPL-2.1 | yes |

GPL-3.0 means Winios ships its source and cannot go to the App Store. That is
unchanged from before and consistent with the sideload target.
